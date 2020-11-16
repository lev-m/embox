
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
#include <kernel/printk.h>
#include <kernel/sched/waitq.h>
#include <kernel/thread/sync/mutex.h>
#include <kernel/thread/waitq.h>
#include <kernel/time/ktime.h>
#include <mem/sysmalloc.h>
#include <module/embox/arch/libarch.h>
#include <util/array.h>
#include <util/log.h>
#include <util/math.h>
#include <util/slist.h>

#define XVBD_DEFAULT_BLOCK_SIZE 512
#define REQUESTS_LIMIT 128

struct request {
	int16_t status;
	struct request** mem;
	struct waitq waitq;
	grant_ref_t grefs[];
};

struct xvbd_ring {
	struct blkif_front_ring ring;
	grant_ref_t gref;
	struct mutex lock;
	struct waitq waitq;
};

struct pool {
	struct request* free_requests;
	unsigned long limit;
	struct waitq wait_for_free;
	struct mutex lock;
	struct mutex pages_lock;
};

struct backend {
	domid_t id;
	struct pool pools[BLKIF_MAX_SEGMENTS_PER_REQUEST];
	
	struct backend* link;
};

struct xvbd {
	size_t block_size;
	blkif_vdev_t id;
	evtchn_port_t port;
	struct xvbd_ring ring;

	struct backend* backend;
	char xs_path[XS_MSG_LEN / 2];
	char xs_backend_path[XS_MSG_LEN / 2];

	struct xvbd* link;
};

static struct xvbd* xvbds = NULL;
static struct backend* backends = NULL;

static struct block_dev_driver xvbd_driver;

static irq_return_t xvbd_irq_handler(unsigned int irq_nr, void *data);

static struct backend* backend_get(domid_t id) {
	struct backend* backend = backends;
	for (; backend != NULL; backend = backend->link) {
		if (backend->id == id) {
			return backend;
		}
	}

	backend = sysmalloc(sizeof(struct backend));
	backend->id = id;
	backend->link = backends;
	
	for (int i = 0; i < BLKIF_MAX_SEGMENTS_PER_REQUEST; i++) {
		struct pool* pool = &backend->pools[i];
		pool->free_requests = NULL;
		pool->limit = REQUESTS_LIMIT;
		waitq_init(&pool->wait_for_free);
		mutex_init(&pool->lock);
		mutex_init(&pool->pages_lock);
	}

	backends = backend;
	return backend;
}

static struct request* backend_get_request(struct backend* backend, int size) {
	struct request* request;
	struct pool* pool = &backend->pools[size - 1];

	mutex_lock(&pool->lock);
	{
		WAITQ_WAIT(&pool->wait_for_free, pool->limit > 0);
		mutex_lock(&pool->pages_lock);
		{
			pool->limit -= 1;
			if (pool->free_requests != NULL) {
				request = pool->free_requests;
				pool->free_requests = *request->mem;
			} else {
				request = sysmalloc(sizeof (struct request) + sizeof (grant_ref_t) * size);
				waitq_init(&request->waitq);
				request->mem = xen_mem_alloc(size);
				char* mem = (char*) request->mem;
				for (int i = 0; i < size; i++, mem += XEN_PAGE_SIZE) {
					request->grefs[i] = gnttab_grant_access(backend->id, mem, false);
				}
			}
		}
		mutex_unlock(&pool->pages_lock);
	}
	mutex_unlock(&pool->lock);

	return request;
}

static void backend_free_request(struct backend* backend, int size, struct request* request) {
	struct pool* pool = &backend->pools[size - 1];

	mutex_lock(&pool->pages_lock);
	{
		pool->limit += 1;
		*request->mem = pool->free_requests;
		pool->free_requests = request;
	}
	mutex_unlock(&pool->pages_lock);

	waitq_wakeup_all(&pool->wait_for_free);
}

static int xvbd_read_xs(struct xvbd* xvbd) {
	int err;

	sprintf(xvbd->xs_path, "device/vbd/%d", xvbd->id);

	domid_t backend_id;
	err = xenstore_scanf(xvbd->xs_path, "backend-id", "%hu", &backend_id);
	if (err != 1) {
		log_error("(%d) fail to read backend id", err);
		return -1;
	}

	err = xenstore_scanf(xvbd->xs_path, "backend", "%s", xvbd->xs_backend_path);
	if (err != 1) {
		log_error("(%d) fail to read backend path", err);
		return -1;
	}

	struct backend* backend = backend_get(backend_id);
	xvbd->backend = backend;

	return 0;
}

static int xvbd_init(struct xvbd* xvbd) {

	{
		struct blkif_sring* shared_ring = (struct blkif_sring*)xen_mem_alloc(1);
		if (shared_ring == NULL) {
			return -ENOMEM;
		}
		SHARED_RING_INIT(shared_ring);
		FRONT_RING_INIT(&xvbd->ring.ring, shared_ring, XEN_PAGE_SIZE);

		xvbd->ring.gref = gnttab_grant_access(xvbd->backend->id, shared_ring, false);
		mutex_init(&xvbd->ring.lock);
		waitq_init(&xvbd->ring.waitq);
	}

	{
		int err;

		err = evtchn_allocate(&xvbd->port, xvbd->backend->id);
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

static int xvbd_write_xs(const struct xvbd* xvbd) {
	int err;

	err = xenstore_printf(xvbd->xs_path, "ring-ref", "%d", xvbd->ring.gref);
	if (err < 0) {
		log_error("fail to write ring-ref");
		return -1;
	}

	err = xenstore_printf(xvbd->xs_path, "event-channel", "%d", xvbd->port);
	if (err < 0) {
		log_error("fail to write event-channel");
		return -1;
	}

	err = xenstore_printf(xvbd->xs_path, "state", "%d", XenbusStateConnected);
	if (err < 0) {
		log_error("fail to write state");
		return -1;
	}

	err = xenstore_printf(xvbd->xs_path, "feature-persistent", "%d", 1);
	if (err < 0) {
		log_error("fail to write feature-persistent");
		return -1;
	}

	err = xenstore_printf(xvbd->xs_path, "feature-large-sector-size", "%d", 1);
	if (err < 0) {
		log_error("fail to write feature-large-sector-size");
		return -1;
	}

	return 0;
}

int xvbd_wait_backend(const struct xvbd* xvbd) {
	int err;
	XenbusState state = 0;

	for (int count = 0; count < 10 && state < XenbusStateConnected; count++) {
		ksleep(1);
		err = xenstore_scanf(xvbd->xs_backend_path, "state", "%u", &state);
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

static int xvbd_create_device(struct xvbd* xvbd) {
	int err;
	char name[16];

	err = xenstore_scanf(xvbd->xs_backend_path, "dev", "%s", name);
	if (err != 1) {
		sprintf(name, "xvd%d", xvbd->id);
	}

	struct block_dev* dev = block_dev_create(name, &xvbd_driver, xvbd);
	if (dev == NULL) {
		return -ENOMEM;
	}

	err = xenstore_scanf(xvbd->xs_backend_path, "sector-size", "%d", &xvbd->block_size);
	if (err != 1) {
		xvbd->block_size = XVBD_DEFAULT_BLOCK_SIZE;
	}

	err = xenstore_scanf(xvbd->xs_backend_path, "physical-sector-size", "%d", &dev->block_size);
	if (err != 1) {
		dev->block_size = xvbd->block_size;
	}

	assert(xvbd->block_size <= XEN_PAGE_SIZE);

	uint64_t sectors_cnt;
	err = xenstore_scanf(xvbd->xs_backend_path, "sectors", "%llu", &sectors_cnt);
	if (err != 1) {
		log_error("fail to read number of blocks");
		return -1;
	}

	dev->size = xvbd->block_size * sectors_cnt;

	return 0;
}

static int xvbd_create(const char* id) {
	struct xvbd* xvbd = sysmalloc(sizeof (struct xvbd));
	xvbd->link = xvbds;
	xvbds = xvbd;
	if (xvbd == NULL) {
		return -ENOMEM;
	}

	xvbd->id = atoi(id);

	int err;

	err = xvbd_read_xs(xvbd);
	if (err != 0) {
		return err;
	}

	err = xvbd_init(xvbd);
	if (err != 0) {
		return err;
	}

	err = xvbd_write_xs(xvbd);
	if (err != 0) {
		return err;
	}

	err = xvbd_wait_backend(xvbd);
	if (err != 0) {
		return err;
	}

	err = xvbd_create_device(xvbd);
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

static int xvbd_do(struct block_dev *dev, char *buffer, size_t count,
		blkno_t blkno, bool write) {

	struct xvbd* xvbd = dev->privdata;
	int pages_cnt = count / XEN_PAGE_SIZE + (count % XEN_PAGE_SIZE != 0);
	struct request* request = backend_get_request(xvbd->backend, pages_cnt);

#ifndef NDEBUG
	memset(request->mem, 0, pages_cnt * XEN_PAGE_SIZE);
#endif

	if (write) {
		memcpy(request->mem, buffer, count);
	}

	request->status = 1;

	mutex_lock(&xvbd->ring.lock);
	{
		WAITQ_WAIT(&xvbd->ring.waitq, RING_FREE_REQUESTS(&xvbd->ring.ring) > 0);

		RING_IDX idx = xvbd->ring.ring.req_prod_pvt++;
		struct blkif_request* ring_request = RING_GET_REQUEST(&xvbd->ring.ring, idx);

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

	WAITQ_WAIT(&request->waitq, request->status <= 0);

	int ret = request->status;
	if (ret == 0) {
		if (!write) {
			memcpy(buffer, request->mem, count);
		}

		ret = count;
	}

	backend_free_request(xvbd->backend, pages_cnt, request);

	return ret;
}

static int xvbd_read(struct block_dev *dev, char *buffer, size_t count, blkno_t blkno) {
	return xvbd_do(dev, buffer, count, blkno, false);
}

static int xvbd_write(struct block_dev *dev, char *buffer, size_t count, blkno_t blkno) {
	return xvbd_do(dev, buffer, count, blkno, true);
}

static irq_return_t xvbd_irq_handler(unsigned int irq_nr, void *data) {
	struct xvbd_ring* ring = data;

	int work_to_do = 1;
	RING_FINAL_CHECK_FOR_RESPONSES(&ring->ring, work_to_do);

	while (work_to_do) {
		RING_IDX consumed_cnt = ring->ring.rsp_cons++;

		struct blkif_response* response = RING_GET_RESPONSE(&ring->ring, consumed_cnt);
		struct request* request = (struct request*)(uintptr_t)response->id;
		request->status = response->status;

		waitq_wakeup_all(&request->waitq);

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
