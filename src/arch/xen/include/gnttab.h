#ifndef GNTTAB_H_
#define GNTTAB_H_

#include <stdint.h>
#include <stdbool.h>
#include <xen/grant_table.h>

grant_ref_t gnttab_grant_access(domid_t domid, void* page, bool readonly);

#endif /* GNTTAB_H_ */
