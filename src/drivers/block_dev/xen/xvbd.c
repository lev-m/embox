
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <xenhelper.h>
#include <xen/io/xenbus.h>
#include <xen/io/ring.h>
#include <xen/io/blkif.h>

#include <drivers/block_dev.h>
#include <framework/mod/options.h>
#include <kernel/irq.h>
#include <kernel/sched/waitq.h>
#include <kernel/thread/sync/mutex.h>
#include <kernel/thread/waitq.h>
#include <mem/misc/pool.h>
#include <util/array.h>
#include <util/log.h>
#include <util/math.h>
#include <util/slist.h>

#define XVBD_DEFAULT_BLOCK_SIZE 512
#define REQUEST_PAGES_CNT BLKIF_MAX_SEGMENTS_PER_REQUEST

#define VBD_REQUESTS_CNT (XEN_PAGE_SIZE / sizeof(struct blkif_request))
#define SHARED_RING_PAGES_CNT 1

#define VBD_REQUEST_PAGES_CNT (REQUEST_PAGES_CNT * VBD_REQUESTS_CNT)
#define VBD_PAGES_CNT (SHARED_RING_PAGES_CNT + VBD_REQUEST_PAGES_CNT)

#define VBD_CNT OPTION_GET(NUMBER, vbd_cnt)

struct xvbd_request {
	int16_t status;
	page_t* pages;
	grant_ref_t grefs[REQUEST_PAGES_CNT];

	struct waitq wait_for_complete;
	struct slist_link link;
};

struct xvbd_ring {
	struct blkif_front_ring ring;
	grant_ref_t gref;
	struct mutex lock;
};

struct xvbd {
	size_t block_size;
	blkif_vdev_t id;
	evtchn_port_t port;

	struct xvbd_ring ring;

	struct mutex wait_lock;
	struct mutex pool_lock;
	struct waitq wait_for_free;
	struct slist free_requests;
	struct xvbd_request requests[VBD_REQUESTS_CNT];
};

struct xvbd_init {
	struct xvbd* xvbd;
	domid_t backend_id;
	char xs_path[XS_MSG_LEN / 2];
	char xs_backend_path[XS_MSG_LEN / 2];
};

POOL_DEF(xvbd_pool, struct xvbd, VBD_CNT);

static struct block_dev_driver xvbd_driver;

static irq_return_t xvbd_irq_handler(unsigned int irq_nr, void *data);

static int xvbd_fill_init(struct xvbd_init* info, struct xvbd* xvbd) {
	int err;

	info->xvbd = xvbd;

	sprintf(info->xs_path, "device/vbd/%d", xvbd->id);

	err = xenstore_scanf(info->xs_path, "backend-id", "%hu", &info->backend_id);
	if (err != 1) {
		log_error("(%d) fail to read backend id", err);
		return -1;
	}

	err = xenstore_scanf(info->xs_path, "backend", "%s", info->xs_backend_path);
	if (err != 1) {
		log_error("(%d) fail to read backend path", err);
		return -1;
	}

	return 0;
}

static int xvbd_init(const struct xvbd_init* info) {
	struct xvbd* xvbd = info->xvbd;

	page_t* pages = xen_mem_alloc(VBD_PAGES_CNT);
	if (pages == NULL) {
		return -ENOMEM;
	}

	{
		struct blkif_sring* shared_ring = (void*)pages;
		SHARED_RING_INIT(shared_ring);
		FRONT_RING_INIT(&xvbd->ring.ring, shared_ring, XEN_PAGE_SIZE);

		xvbd->ring.gref = gnttab_grant_access(info->backend_id, pages, 0);
		pages++;
		mutex_init(&xvbd->ring.lock);
	}

	{
		slist_init(&xvbd->free_requests);

		struct xvbd_request* request;
		array_foreach_ptr(request, xvbd->requests, VBD_REQUESTS_CNT) {
			request->pages = pages;

			grant_ref_t* gref;
			array_foreach_ptr(gref, request->grefs, REQUEST_PAGES_CNT) {
				*gref = gnttab_grant_access(info->backend_id, pages, 0);
				pages++;
			}

			waitq_init(&request->wait_for_complete);
			slist_link_init(&request->link);
			slist_add_first_element(request, &xvbd->free_requests, link);
		}

		mutex_init(&xvbd->wait_lock);
		mutex_init(&xvbd->pool_lock);
		waitq_init(&xvbd->wait_for_free);
	}

	{
		int err;

		err = evtchn_allocate(&xvbd->port, info->backend_id);
		if (err != 0) {
			log_error("fail to allocate event channel");
			return err;
		}

		err = irq_attach(xvbd->port, xvbd_irq_handler, 0, &xvbd->ring, NULL);
		if (err != 0) {
			log_error("fail to attach handler to event channel");
			return err;
		}
	}

	return 0;
}

static int xvbd_write_xs(const struct xvbd_init* info) {
	struct xvbd* xvbd = info->xvbd;
	int err;

	err = xenstore_printf(info->xs_path, "ring-ref", "%d", xvbd->ring.gref);
	if (err < 0) {
		log_error("fail to write ring-ref");
		return -1;
	}

	err = xenstore_printf(info->xs_path, "event-channel", "%d", xvbd->port);
	if (err < 0) {
		log_error("fail to write event-channel");
		return -1;
	}

	err = xenstore_printf(info->xs_path, "state", "%d", XenbusStateConnected);
	if (err < 0) {
		log_error("fail to write state");
		return -1;
	}

	err = xenstore_printf(info->xs_path, "feature-persistent", "%d", 1);
	if (err < 0) {
		log_error("fail to write feature-persistent");
		return -1;
	}

	err = xenstore_printf(info->xs_path, "feature-large-sector-size", "%d", 1);
	if (err < 0) {
		log_error("fail to write feature-large-sector-size");
		return -1;
	}

	return 0;
}

int xvbd_wait_backend(const struct xvbd_init* info) {
	int err;
	XenbusState state = 0;

	for (int count = 0; count < 500 && state < XenbusStateConnected; count++) {
		err = xenstore_scanf(info->xs_backend_path, "state", "%u", &state);
		if (err < 0) {
			log_error("fail to read backend state");
			return -1;
		}
	}

	if (state != XenbusStateConnected) {
		log_error("backend is not initialied");
		return -1;
	}

	return 0;
}

static int xvbd_create_device(const struct xvbd_init* info) {
	struct xvbd* xvbd = info->xvbd;

	int err;
	char name[16];

	err = xenstore_scanf(info->xs_backend_path, "dev", "%s", name);
	if (err != 1) {
		sprintf(name, "xvd%d", info->xvbd->id);
	}

	struct block_dev* dev = block_dev_create(name, &xvbd_driver, xvbd);
	if (dev == NULL) {
		return -ENOMEM;
	}

	err = xenstore_scanf(info->xs_backend_path, "sector-size", "%d", &xvbd->block_size);
	if (err != 1) {
		xvbd->block_size = XVBD_DEFAULT_BLOCK_SIZE;
	}

	err = xenstore_scanf(info->xs_backend_path, "physical-sector-size", "%d", &dev->block_size);
	if (err != 1) {
		dev->block_size = xvbd->block_size;
	}

	assert(xvbd->block_size <= XEN_PAGE_SIZE);

	uint64_t sectors_cnt;
	err = xenstore_scanf(info->xs_backend_path, "sectors", "%llu", &sectors_cnt);
	if (err != 1) {
		log_error("fail to read number of blocks");
		return -1;
	}

	dev->size = xvbd->block_size * sectors_cnt;

	return 0;
}

static int xvbd_create(const char* id) {
	struct xvbd* xvbd = pool_alloc(&xvbd_pool);
	if (xvbd == NULL) {
		return -ENOMEM;
	}

	xvbd->id = atoi(id);

	int err;
	struct xvbd_init info;

	err = xvbd_fill_init(&info, xvbd);
	if (err != 0) {
		return err;
	}

	err = xvbd_init(&info);
	if (err != 0) {
		return err;
	}

	err = xvbd_write_xs(&info);
	if (err != 0) {
		return err;
	}

	err = xvbd_wait_backend(&info);
	if (err != 0) {
		return err;
	}

	err = xvbd_create_device(&info);
	if (err != 0) {
		return err;
	}

	return 0;
}

static int xvbd_probe(void* args) {
	int err;

	xenstore_foreach("device/vbd", id) {
		if (0 != (err = xvbd_create(id))) {
			return err;
		}
	}

	return 0;
}

static inline struct xvbd_request* xvbd_get_request(struct xvbd* xvbd) {
	struct xvbd_request* request;

	mutex_lock(&xvbd->wait_lock);
	{
		WAITQ_WAIT(&xvbd->wait_for_free, !slist_empty(&xvbd->free_requests));

		mutex_lock(&xvbd->pool_lock);
		{
			request = slist_remove_first_element(&xvbd->free_requests,
					struct xvbd_request, link);
		}
		mutex_unlock(&xvbd->pool_lock);
	}
	mutex_unlock(&xvbd->wait_lock);

	return request;
}

static inline void xvbd_free_request(struct xvbd* xvbd, struct xvbd_request* request) {
	mutex_lock(&xvbd->pool_lock);
	{
		slist_add_first_element(request, &xvbd->free_requests, link);
	}
	mutex_unlock(&xvbd->pool_lock);

	waitq_wakeup_all(&xvbd->wait_for_free);
}

static int xvbd_do(struct block_dev *dev, char *buffer, size_t count,
		blkno_t blkno, int write) {
	struct xvbd* xvbd = dev->privdata;
	struct xvbd_request* request = xvbd_get_request(xvbd);
	request->status = 1;

#ifndef NDEBUG
	memset(request->pages, 0, REQUEST_PAGES_CNT * sizeof(page_t));
#endif

	if (write) {
		memcpy(request->pages, buffer, count);
	}

	mutex_lock(&xvbd->ring.lock);
	{
		RING_IDX idx = xvbd->ring.ring.req_prod_pvt++;
		struct blkif_request* ring_request = RING_GET_REQUEST(&xvbd->ring.ring, idx);

		int pages_cnt = count / XEN_PAGE_SIZE + (count % XEN_PAGE_SIZE != 0);
		ring_request->operation = write ? BLKIF_OP_WRITE : BLKIF_OP_READ;
		ring_request->nr_segments = pages_cnt;
		ring_request->handle = xvbd->id;
		ring_request->sector_number = blkno * dev->block_size / xvbd->block_size;
		ring_request->id = (uint64_t)(uintptr_t)request;

		uint8_t last_sect = (XEN_PAGE_SIZE - 1) / xvbd->block_size;

		for (int i = 0; i < pages_cnt; i++) {
			ring_request->seg[i].first_sect = 0;
			ring_request->seg[i].last_sect = last_sect;
			ring_request->seg[i].gref = request->grefs[i];
		}
			
		ring_request->seg[pages_cnt - 1].last_sect =
				((count - 1) % XEN_PAGE_SIZE) / xvbd->block_size;

		int notify;
		RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&xvbd->ring.ring, notify);

		if (notify) {
			evtchn_notify_remote(xvbd->port);
		}
	}
	mutex_unlock(&xvbd->ring.lock);

	WAITQ_WAIT(&request->wait_for_complete, request->status <= 0);

	int ret = request->status;
	if (ret == 0) {
		if (!write) {
			memcpy(buffer, request->pages, count);
		}

		ret = count;
	}

	xvbd_free_request(xvbd, request);

	return ret;
}

static int xvbd_read(struct block_dev *dev, char *buffer, size_t count, blkno_t blkno) {
	return xvbd_do(dev, buffer, count, blkno, 0);
}

static int xvbd_write(struct block_dev *dev, char *buffer, size_t count, blkno_t blkno) {
	return xvbd_do(dev, buffer, count, blkno, 1);
}

static irq_return_t xvbd_irq_handler(unsigned int irq_nr, void *data) {
	struct xvbd_ring* ring = data;

	int work_to_do = 1;
	RING_FINAL_CHECK_FOR_RESPONSES(&ring->ring, work_to_do);

	while (work_to_do) {
		RING_IDX consumed_cnt = ring->ring.rsp_cons++;

		struct blkif_response* response = RING_GET_RESPONSE(&ring->ring, consumed_cnt);
		struct xvbd_request* request = (struct xvbd_request*)(uintptr_t)response->id;
		request->status = response->status;

		waitq_wakeup_all(&request->wait_for_complete);

		RING_FINAL_CHECK_FOR_RESPONSES(&ring->ring, work_to_do);
	}

	return IRQ_HANDLED;
}

static struct block_dev_driver xvbd_driver = {
	.name = "xvbd",

	.ioctl = NULL,
	.read = xvbd_read,
	.write = xvbd_write,

	.probe = xvbd_probe,
};

BLOCK_DEV_DEF("xvbd", &xvbd_driver);
