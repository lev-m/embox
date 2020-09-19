#ifndef HYPERCALLS_H_
#define HYPERCALLS_H_

#include <stdint.h>

#if defined(__i386__)
#include <xen_hypercall-x86_32.h>
#else
#error "Unsupported architecture"
#endif

#endif /* HYPERCALLS_H_ */
