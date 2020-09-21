
#include <xenhelper.h>
#include <xen/io/ring.h>

#include <embox/unit.h>
#include <kernel/printk.h>

EMBOX_UNIT_INIT(gnttab_init);

static grant_ref_t ref = GNTTAB_NR_RESERVED_ENTRIES;
static grant_entry_v1_t *grant_table;

grant_ref_t gnttab_grant_access(domid_t domid, void* frame, bool readonly) {
	grant_table[ref].frame = virt_to_mfn(frame);
	grant_table[ref].domid = domid;
	wmb();
	grant_table[ref].flags = GTF_permit_access | (GTF_readonly * readonly);
	return ref++;
}

int get_max_nr_grant_frames() {
	struct gnttab_query_size query;
	
	int rc;
	query.dom = DOMID_SELF;
	rc = HYPERVISOR_grant_table_op(GNTTABOP_query_size, &query, 1);
	if ((rc < 0) || (query.status != GNTST_okay))
		return 4; /* Legacy max supported number of frames */
	return query.max_nr_frames;
}

static int gnttab_init(void) {
	int frames_cnt = get_max_nr_grant_frames();
	//size_t entries_cnt = frames_cnt * XEN_PAGE_SIZE / sizeof(grant_entry_v1_t);

	struct gnttab_setup_table setup;
	setup.dom = DOMID_SELF;
	setup.nr_frames = frames_cnt;
	unsigned long frames[frames_cnt];
	set_xen_guest_handle(setup.frame_list, frames);

	int rc;
	rc = HYPERVISOR_grant_table_op(GNTTABOP_setup_table, &setup, 1);
	if (rc < 0) {
		return rc;
	}
	unsigned long va = (unsigned long)xen_mem_alloc(frames_cnt);

	int count;    
	for (count = 0; count < frames_cnt; count++)
	{
		rc = HYPERVISOR_update_va_mapping(va + count * XEN_PAGE_SIZE,
				__pte((frames[count]<< XEN_PAGE_SHIFT) | 7),
				UVMF_INVLPG);
		if (rc < 0) {
			return rc;
		}
	}

	grant_table = (void*)va;
	return 0;
}
