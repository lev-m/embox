#ifndef EVTCHN_H_
#define EVTCHN_H_

#include <hypercalls.h>
#include <xen/event_channel.h>

static inline int evtchn_notify_remote(evtchn_port_t port) {
	evtchn_send_t op ={
		.port = port,
	};
	return HYPERVISOR_event_channel_op(EVTCHNOP_send, &op);
}

static inline int evtchn_allocate(evtchn_port_t* port, domid_t remote) {
	struct evtchn_alloc_unbound alloc_unbound = {
		.dom = DOMID_SELF,
		.remote_dom = remote,
	};

	int err = HYPERVISOR_event_channel_op(EVTCHNOP_alloc_unbound, &alloc_unbound);
	if (err < 0) {
		return err;
	}

	*port = alloc_unbound.port;
	return 0;
}

#endif /* EVTCHN_H_ */
