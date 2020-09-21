/**
 * @file    xen_memory.h
 * @brief
 *
 * @author  Aleksei Cherepanov
 * @date    30.03.2020
 */

#ifndef MEMORY_H_
#define MEMORY_H_

#include <unistd.h>
#include <xen/io/ring.h>

#define XEN_PAGE_SIZE (1 << XEN_PAGE_SHIFT)

typedef char page_t[XEN_PAGE_SIZE];

extern page_t* xen_mem_alloc(size_t page_number);
extern void xen_mem_free(page_t* pages, size_t page_number);
extern long virt_to_mfn(void* virt);

#endif /* MEMORY_H_ */
