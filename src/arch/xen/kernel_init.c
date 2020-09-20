/**
 * @file
 * @brief 2nd level boot on Xen
 *
 * @date    04.11.2016
 * @author  Andrey Golikov
 * @author  Anton Kozlov
 */

#include <hypercalls.h>
#include <info.h>

/* Embox interface */
extern void kernel_start(void);

/* Xen interface */
extern void trap_init(void);

struct start_info xen_start_info;

shared_info_t *HYPERVISOR_shared_info;

void xen_kernel_start(start_info_t * start_info) {
	HYPERVISOR_update_va_mapping((unsigned long) &xen_shared_info,
			__pte(start_info->shared_info | 7),
			UVMF_INVLPG);

	xen_start_info = *start_info;
	HYPERVISOR_shared_info = &xen_shared_info;

	trap_init();

	kernel_start();
}
