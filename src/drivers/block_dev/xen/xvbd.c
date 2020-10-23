
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

struct page_info {
	unsigned long* page;
	grant_ref_t gref;
};

struct request {
	int16_t status;
	struct waitq waitq;
};

struct xvbd_ring {
	struct blkif_front_ring ring;
	grant_ref_t gref;
	unsigned long user;
};

struct backend {
	domid_t id;
	struct page_info* free_pages;
	unsigned long limit;
	unsigned long waiters;
	struct waitq wait_for_free;
	struct waitq wait_for_enter;
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

	backend = sysmalloc(sizeof(backend));
	backend->id = id;
	backend->link = backends;
	backend->limit = 64;
	backend->free_pages = NULL;
	backend->waiters = 0;
	waitq_init(&backend->wait_for_free);
	waitq_init(&backend->wait_for_enter);
	backends = backend;
	return backend;
}

static void backend_get_pages(struct backend* backend, struct page_info* pages[], unsigned long request_size) {
	WAITQ_WAIT(&backend->wait_for_enter, backend->waiters == 0);

	bool is_waiter = true;
	for (unsigned long lim = backend->limit; lim > request_size; lim = backend->limit) {
		if (cas(&backend->limit, lim, lim - request_size)) {
			is_waiter = false;
			break;
		}
	}

	if (is_waiter) {
		unsigned long waiters;
		do {
			waiters = backend->waiters;
		} while (!cas(&backend->waiters, waiters, waiters + 1));

		while (true) {
			WAITQ_WAIT_ONCE(&backend->wait_for_free, SCHED_TIMEOUT_INFINITE);
			for (unsigned long lim = backend->limit; lim > request_size; lim = backend->limit) {
				if (cas(&backend->limit, lim, lim - request_size)) {
					break;
				}
			}
		}
	}

	for (int i = 0; i < request_size; i++) {
		struct page_info* info;

		while (true) {
			info = backend->free_pages;

			if (info != NULL) {
				if (cas(&backend->free_pages, info, *info->page)) {
					break;
				}
			} else {
				info = malloc(sizeof (struct page_info));
				info->page = xen_mem_alloc(1);
				info->gref = gnttab_grant_access(backend->id, info->page, false);
				break;
			}
		}

		pages[i] = info;
	}

	unsigned long lim;
	do {
		lim = backend->limit;
	} while (!cas(&backend->limit, lim, lim + request_size));

	if (is_waiter) {
		unsigned long waiters;
		do {
			waiters = backend->waiters;
		} while (!cas(&backend->waiters, waiters, waiters - 1));

		if (waiters == 0) {
			waitq_wakeup_all(&backend->wait_for_enter);
		}
	}
}

static void backend_free_pages(struct backend* backend, struct page_info* pages[], int request_size) {
	unsigned long* place = (unsigned long*) &backend->free_pages;
	for (int i = 0; i < request_size; i++) {
		struct page_info* info = pages[i];
		unsigned long* link = info->page;
		unsigned long value = (unsigned long) pages[i];
		unsigned long old;
		do {
			old = *place;
			*link = old;
		} while (!cas(place, old, value));
	}

	waitq_wakeup_all(&backend->wait_for_free);
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
		xvbd->ring.user = 0;
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
	struct page_info* pages[pages_cnt];
	backend_get_pages(xvbd->backend, pages, pages_cnt);

#ifndef NDEBUG
	for (int i = 0; i < pages_cnt; i++) {
		memset(pages[i]->page, 0, XEN_PAGE_SIZE);
	}
#endif

	if (write) {
		size_t offset = 0;
		for (int i = 0; i < pages_cnt - 1; i++) {
			memcpy(pages[i]->page, buffer + offset, XEN_PAGE_SIZE);
			offset += XEN_PAGE_SIZE;
		}
		memcpy(pages[pages_cnt - 1]->page, buffer + offset, count & (XEN_PAGE_SIZE - 1));
	}

	struct request request;
	request.status = 1;
	waitq_init(&request.waitq);

	unsigned long user = (unsigned long)&request;
	while (!cas(&xvbd->ring.user, 0, user)) {
		// DO NOTHING
	}

	{
		RING_IDX idx = xvbd->ring.ring.req_prod_pvt++;
		struct blkif_request* ring_request = RING_GET_REQUEST(&xvbd->ring.ring, idx);

		ring_request->operation = write ? BLKIF_OP_WRITE : BLKIF_OP_READ;
		ring_request->nr_segments = pages_cnt;
		ring_request->handle = xvbd->id;
		ring_request->sector_number = blkno * dev->block_size / xvbd->block_size;
		ring_request->id = (uint64_t)user;

		uint8_t last_sect = (XEN_PAGE_SIZE - 1) / xvbd->block_size;

		for (int i = 0; i < pages_cnt; i++) {
			ring_request->seg[i].first_sect = 0;
			ring_request->seg[i].last_sect = last_sect;
			ring_request->seg[i].gref = pages[i]->gref;
		}
			
		ring_request->seg[pages_cnt - 1].last_sect =
				((count - 1) % XEN_PAGE_SIZE) / xvbd->block_size;

		int notify;
		RING_PUSH_REQUESTS_AND_CHECK_NOTIFY(&xvbd->ring.ring, notify);

		if (notify) {
			evtchn_notify_remote(xvbd->port);
		}
	}
	xvbd->ring.user = 0;

	WAITQ_WAIT_ONCE(&request.waitq, SCHED_TIMEOUT_INFINITE);

	int ret = request.status;
	if (ret == 0) {
		if (!write) {
			size_t offset = 0;
			for (int i = 0; i < pages_cnt - 1; i++) {
				memcpy(buffer + offset, pages[i]->page, XEN_PAGE_SIZE);
				offset += XEN_PAGE_SIZE;
			}
			memcpy(buffer + offset, pages[pages_cnt - 1]->page, count & (XEN_PAGE_SIZE - 1));
		}

		ret = count;
	}

	backend_free_pages(xvbd->backend, pages, pages_cnt);

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
		struct request* request = (struct request*)(unsigned long)response->id;
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
