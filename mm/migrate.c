// SPDX-License-Identifier: GPL-2.0
/*
 * Memory Migration functionality - linux/mm/migrate.c
 *
 * Copyright (C) 2006 Silicon Graphics, Inc., Christoph Lameter
 *
 * Page migration was first developed in the context of the memory hotplug
 * project. The main authors of the migration code are:
 *
 * IWAMOTO Toshihiro <iwamoto@valinux.co.jp>
 * Hirokazu Takahashi <taka@valinux.co.jp>
 * Dave Hansen <haveblue@us.ibm.com>
 * Christoph Lameter
 */

#include <linux/migrate.h>
#include <linux/export.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/pagemap.h>
#include <linux/buffer_head.h>
#include <linux/mm_inline.h>
#include <linux/nsproxy.h>
#include <linux/pagevec.h>
#include <linux/ksm.h>
#include <linux/rmap.h>
#include <linux/topology.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/writeback.h>
#include <linux/mempolicy.h>
#include <linux/vmalloc.h>
#include <linux/security.h>
#include <linux/backing-dev.h>
#include <linux/compaction.h>
#include <linux/syscalls.h>
#include <linux/compat.h>
#include <linux/hugetlb.h>
#include <linux/hugetlb_cgroup.h>
#include <linux/gfp.h>
#include <linux/pagewalk.h>
#include <linux/pfn_t.h>
#include <linux/memremap.h>
#include <linux/userfaultfd_k.h>
#include <linux/balloon_compaction.h>
#include <linux/mmu_notifier.h>
#include <linux/page_idle.h>
#include <linux/page_owner.h>
#include <linux/sched/mm.h>
#include <linux/ptrace.h>
#include <linux/oom.h>

#include <asm/tlbflush.h>

#define CREATE_TRACE_POINTS
#include <trace/events/migrate.h>

#include "internal.h"

int accel_page_copy = 1;
// controls if RPDAA is enabled
int sysctl_enable_page_migration_optimization_avoid_remote_pmem_write = 0;
// for PMEM memory only NUMA node x, CLOSEST_CPU_NODE_FOR_PMEM[x] stores 
// id of CPU which is on the same socket x
// Otherwise it stores -1
int CLOSEST_CPU_NODE_FOR_PMEM[MAX_NUMNODES];
// suggest if CLOSEST_CPU_NODE_FOR_PMEM needs to be updated
int CLOSEST_CPU_NODE_FOR_PMEM_INITIALIZED=0;
EXPORT_SYMBOL(CLOSEST_CPU_NODE_FOR_PMEM_INITIALIZED);

struct page_migration_work_item {
	struct list_head list;
	struct page *old_page;
	struct page *new_page;
	struct anon_vma *anon_vma;
	int page_was_mapped;
};

/*
 * migrate_prep() needs to be called before we start compiling a list of pages
 * to be migrated using isolate_lru_page(). If scheduling work on other CPUs is
 * undesirable, use migrate_prep_local()
 */
int migrate_prep(void)
{
	/*
	 * Clear the LRU lists so pages can be isolated.
	 * Note that pages may be moved off the LRU after we have
	 * drained them. Those pages will fail to migrate like other
	 * pages that may be busy.
	 */
	lru_add_drain_all();

	return 0;
}

/* Do the necessary work of migrate_prep but not if it involves other CPUs */
int migrate_prep_local(void)
{
	lru_add_drain();

	return 0;
}

int isolate_movable_page(struct page *page, isolate_mode_t mode)
{
	struct address_space *mapping;

	/*
	 * Avoid burning cycles with pages that are yet under __free_pages(),
	 * or just got freed under us.
	 *
	 * In case we 'win' a race for a movable page being freed under us and
	 * raise its refcount preventing __free_pages() from doing its job
	 * the put_page() at the end of this block will take care of
	 * release this page, thus avoiding a nasty leakage.
	 */
	if (unlikely(!get_page_unless_zero(page)))
		goto out;

	/*
	 * Check PageMovable before holding a PG_lock because page's owner
	 * assumes anybody doesn't touch PG_lock of newly allocated page
	 * so unconditionally grabbing the lock ruins page's owner side.
	 */
	if (unlikely(!__PageMovable(page)))
		goto out_putpage;
	/*
	 * As movable pages are not isolated from LRU lists, concurrent
	 * compaction threads can race against page migration functions
	 * as well as race against the releasing a page.
	 *
	 * In order to avoid having an already isolated movable page
	 * being (wrongly) re-isolated while it is under migration,
	 * or to avoid attempting to isolate pages being released,
	 * lets be sure we have the page lock
	 * before proceeding with the movable page isolation steps.
	 */
	if (unlikely(!trylock_page(page)))
		goto out_putpage;

	if (!PageMovable(page) || PageIsolated(page))
		goto out_no_isolated;

	mapping = page_mapping(page);
	VM_BUG_ON_PAGE(!mapping, page);

	if (!mapping->a_ops->isolate_page(page, mode))
		goto out_no_isolated;

	/* Driver shouldn't use PG_isolated bit of page->flags */
	WARN_ON_ONCE(PageIsolated(page));
	__SetPageIsolated(page);
	unlock_page(page);

	return 0;

out_no_isolated:
	unlock_page(page);
out_putpage:
	put_page(page);
out:
	return -EBUSY;
}

/* It should be called on page which is PG_movable */
void putback_movable_page(struct page *page)
{
	struct address_space *mapping;

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(!PageMovable(page), page);
	VM_BUG_ON_PAGE(!PageIsolated(page), page);

	mapping = page_mapping(page);
	mapping->a_ops->putback_page(page);
	__ClearPageIsolated(page);
}

/*
 * Put previously isolated pages back onto the appropriate lists
 * from where they were once taken off for compaction/migration.
 *
 * This function shall be used whenever the isolated pageset has been
 * built from lru, balloon, hugetlbfs page. See isolate_migratepages_range()
 * and isolate_huge_page().
 */
void putback_movable_pages(struct list_head *l)
{
	struct page *page;
	struct page *page2;

	list_for_each_entry_safe(page, page2, l, lru) {
		if (unlikely(PageHuge(page))) {
			putback_active_hugepage(page);
			continue;
		}
		list_del(&page->lru);
		/*
		 * We isolated non-lru movable page so here we can use
		 * __PageMovable because LRU page's mapping cannot have
		 * PAGE_MAPPING_MOVABLE.
		 */
		if (unlikely(__PageMovable(page))) {
			VM_BUG_ON_PAGE(!PageIsolated(page), page);
			lock_page(page);
			if (PageMovable(page))
				putback_movable_page(page);
			else
				__ClearPageIsolated(page);
			unlock_page(page);
			put_page(page);
		} else {
			mod_node_page_state(page_pgdat(page), NR_ISOLATED_ANON +
					page_is_file_cache(page), -hpage_nr_pages(page));
			putback_lru_page(page);
		}
	}
}

/*
 * Restore a potential migration pte to a working pte entry
 */
static bool remove_migration_pte(struct page *page, struct vm_area_struct *vma,
				 unsigned long addr, void *old)
{
	struct page_vma_mapped_walk pvmw = {
		.page = old,
		.vma = vma,
		.address = addr,
		.flags = PVMW_SYNC | PVMW_MIGRATION,
	};
	struct page *new;
	pte_t pte;
	swp_entry_t entry;

	VM_BUG_ON_PAGE(PageTail(page), page);
	while (page_vma_mapped_walk(&pvmw)) {
		if (PageKsm(page))
			new = page;
		else
			new = page - page->index +
				linear_page_index(vma, pvmw.address);

#ifdef CONFIG_ARCH_ENABLE_THP_MIGRATION
		/* PMD-mapped THP migration entry */
		if (!pvmw.pte) {
			VM_BUG_ON_PAGE(PageHuge(page) || !PageTransCompound(page), page);
			remove_migration_pmd(&pvmw, new);
			continue;
		}
#endif

		get_page(new);
		pte = pte_mkold(mk_pte(new, READ_ONCE(vma->vm_page_prot)));
		if (pte_swp_soft_dirty(*pvmw.pte))
			pte = pte_mksoft_dirty(pte);

		/*
		 * Recheck VMA as permissions can change since migration started
		 */
		entry = pte_to_swp_entry(*pvmw.pte);
		if (is_write_migration_entry(entry))
			pte = maybe_mkwrite(pte, vma);

		if (unlikely(is_zone_device_page(new))) {
			if (is_device_private_page(new)) {
				entry = make_device_private_entry(new, pte_write(pte));
				pte = swp_entry_to_pte(entry);
			}
		}

#ifdef CONFIG_HUGETLB_PAGE
		if (PageHuge(new)) {
			pte = pte_mkhuge(pte);
			pte = arch_make_huge_pte(pte, vma, new, 0);
			set_huge_pte_at(vma->vm_mm, pvmw.address, pvmw.pte, pte);
			if (PageAnon(new))
				hugepage_add_anon_rmap(new, vma, pvmw.address);
			else
				page_dup_rmap(new, true);
		} else
#endif
		{
			set_pte_at(vma->vm_mm, pvmw.address, pvmw.pte, pte);

			if (PageAnon(new))
				page_add_anon_rmap(new, vma, pvmw.address, false);
			else
				page_add_file_rmap(new, false);
		}
		if (vma->vm_flags & VM_LOCKED && !PageTransCompound(new))
			mlock_vma_page(new);

		if (PageTransHuge(page) && PageMlocked(page))
			clear_page_mlock(page);

		/* No need to invalidate - it was non-present before */
		update_mmu_cache(vma, pvmw.address, pvmw.pte);
	}

	return true;
}

/*
 * Get rid of all migration entries and replace them by
 * references to the indicated page.
 */
void remove_migration_ptes(struct page *old, struct page *new, bool locked)
{
	struct rmap_walk_control rwc = {
		.rmap_one = remove_migration_pte,
		.arg = old,
	};

	if (locked)
		rmap_walk_locked(new, &rwc);
	else
		rmap_walk(new, &rwc);
}

/*
 * Something used the pte of a page under migration. We need to
 * get to the page and wait until migration is finished.
 * When we return from this function the fault will be retried.
 */
void __migration_entry_wait(struct mm_struct *mm, pte_t *ptep,
				spinlock_t *ptl)
{
	pte_t pte;
	swp_entry_t entry;
	struct page *page;

	spin_lock(ptl);
	pte = *ptep;
	if (!is_swap_pte(pte))
		goto out;

	entry = pte_to_swp_entry(pte);
	if (!is_migration_entry(entry))
		goto out;

	page = migration_entry_to_page(entry);

	/*
	 * Once page cache replacement of page migration started, page_count
	 * is zero; but we must not call put_and_wait_on_page_locked() without
	 * a ref. Use get_page_unless_zero(), and just fault again if it fails.
	 */
	if (!get_page_unless_zero(page))
		goto out;
	pte_unmap_unlock(ptep, ptl);
	put_and_wait_on_page_locked(page);
	return;
out:
	pte_unmap_unlock(ptep, ptl);
}

void migration_entry_wait(struct mm_struct *mm, pmd_t *pmd,
				unsigned long address)
{
	unsigned long enter_jiffies = jiffies;
	struct task_struct *tsk;

	spinlock_t *ptl = pte_lockptr(mm, pmd);
	pte_t *ptep = pte_offset_map(pmd, address);
	__migration_entry_wait(mm, ptep, ptl);

	enter_jiffies = jiffies - enter_jiffies;
	rcu_read_lock();
	tsk = rcu_dereference(mm->owner);
	rcu_read_unlock();
	tsk->page_migration_stats.base_page_under_migration_jiffies +=
		enter_jiffies;
}

void migration_entry_wait_huge(struct vm_area_struct *vma,
		struct mm_struct *mm, pte_t *pte)
{
	spinlock_t *ptl = huge_pte_lockptr(hstate_vma(vma), mm, pte);
	__migration_entry_wait(mm, pte, ptl);
}

#ifdef CONFIG_ARCH_ENABLE_THP_MIGRATION
void pmd_migration_entry_wait(struct mm_struct *mm, pmd_t *pmd)
{
	spinlock_t *ptl;
	struct page *page;
	unsigned long enter_jiffies = jiffies;
	struct task_struct *tsk;

	ptl = pmd_lock(mm, pmd);
	if (!is_pmd_migration_entry(*pmd))
		goto unlock;
	page = migration_entry_to_page(pmd_to_swp_entry(*pmd));
	if (!get_page_unless_zero(page))
		goto unlock;
	spin_unlock(ptl);
	put_and_wait_on_page_locked(page);

	enter_jiffies = jiffies - enter_jiffies;
	rcu_read_lock();
	tsk = rcu_dereference(mm->owner);
	rcu_read_unlock();
	tsk->page_migration_stats.huge_page_under_migration_jiffies +=
		enter_jiffies;
	return;
unlock:
	spin_unlock(ptl);

	enter_jiffies = jiffies - enter_jiffies;
	rcu_read_lock();
	tsk = rcu_dereference(mm->owner);
	rcu_read_unlock();
	tsk->page_migration_stats.huge_page_under_migration_jiffies +=
		enter_jiffies;
}
#endif

static int expected_page_refs(struct address_space *mapping, struct page *page)
{
	int expected_count = 1;

	/*
	 * Device public or private pages have an extra refcount as they are
	 * ZONE_DEVICE pages.
	 */
	expected_count += is_device_private_page(page);
	if (mapping)
		expected_count += hpage_nr_pages(page) + page_has_private(page);

	return expected_count;
}

/*
 * Replace the page in the mapping.
 *
 * The number of remaining references must be:
 * 1 for anonymous pages without a mapping
 * 2 for pages with a mapping
 * 3 for pages with a mapping and PagePrivate/PagePrivate2 set.
 */
int migrate_page_move_mapping(struct address_space *mapping,
		struct page *newpage, struct page *page, int extra_count)
{
	XA_STATE(xas, &mapping->i_pages, page_index(page));
	struct zone *oldzone, *newzone;
	int dirty;
	int expected_count = expected_page_refs(mapping, page) + extra_count;

	if (!mapping) {
		/* Anonymous page without mapping */
		if (page_count(page) != expected_count)
			return -EAGAIN;

		/* No turning back from here */
		newpage->index = page->index;
		newpage->mapping = page->mapping;
		if (PageSwapBacked(page))
			__SetPageSwapBacked(newpage);

		return MIGRATEPAGE_SUCCESS;
	}

	oldzone = page_zone(page);
	newzone = page_zone(newpage);

	xas_lock_irq(&xas);
	if (page_count(page) != expected_count || xas_load(&xas) != page) {
		xas_unlock_irq(&xas);
		return -EAGAIN;
	}

	if (!page_ref_freeze(page, expected_count)) {
		xas_unlock_irq(&xas);
		return -EAGAIN;
	}

	/*
	 * Now we know that no one else is looking at the page:
	 * no turning back from here.
	 */
	newpage->index = page->index;
	newpage->mapping = page->mapping;
	page_ref_add(newpage, hpage_nr_pages(page)); /* add cache reference */
	if (PageSwapBacked(page)) {
		__SetPageSwapBacked(newpage);
		if (PageSwapCache(page)) {
			SetPageSwapCache(newpage);
			set_page_private(newpage, page_private(page));
		}
	} else {
		VM_BUG_ON_PAGE(PageSwapCache(page), page);
	}

	/* Move dirty while page refs frozen and newpage not yet exposed */
	dirty = PageDirty(page);
	if (dirty) {
		ClearPageDirty(page);
		SetPageDirty(newpage);
	}

	xas_store(&xas, newpage);
	if (PageTransHuge(page)) {
		int i;

		for (i = 1; i < HPAGE_PMD_NR; i++) {
			xas_next(&xas);
			xas_store(&xas, newpage);
		}
	}

	/*
	 * Drop cache reference from old page by unfreezing
	 * to one less reference.
	 * We know this isn't the last reference.
	 */
	page_ref_unfreeze(page, expected_count - hpage_nr_pages(page));

	xas_unlock(&xas);
	/* Leave irq disabled to prevent preemption while updating stats */

	/*
	 * If moved to a different zone then also account
	 * the page for that zone. Other VM counters will be
	 * taken care of when we establish references to the
	 * new page and drop references to the old page.
	 *
	 * Note that anonymous pages are accounted for
	 * via NR_FILE_PAGES and NR_ANON_MAPPED if they
	 * are mapped to swap space.
	 */
	if (newzone != oldzone) {
		__dec_node_state(oldzone->zone_pgdat, NR_FILE_PAGES);
		__inc_node_state(newzone->zone_pgdat, NR_FILE_PAGES);
		if (PageSwapBacked(page) && !PageSwapCache(page)) {
			__dec_node_state(oldzone->zone_pgdat, NR_SHMEM);
			__inc_node_state(newzone->zone_pgdat, NR_SHMEM);
		}
		if (dirty && mapping_cap_account_dirty(mapping)) {
			__dec_node_state(oldzone->zone_pgdat, NR_FILE_DIRTY);
			__dec_zone_state(oldzone, NR_ZONE_WRITE_PENDING);
			__inc_node_state(newzone->zone_pgdat, NR_FILE_DIRTY);
			__inc_zone_state(newzone, NR_ZONE_WRITE_PENDING);
		}
	}
	local_irq_enable();

	return MIGRATEPAGE_SUCCESS;
}
EXPORT_SYMBOL(migrate_page_move_mapping);

/*
 * The expected number of remaining references is the same as that
 * of migrate_page_move_mapping().
 */
int migrate_huge_page_move_mapping(struct address_space *mapping,
				   struct page *newpage, struct page *page)
{
	XA_STATE(xas, &mapping->i_pages, page_index(page));
	int expected_count;

	xas_lock_irq(&xas);
	expected_count = 2 + page_has_private(page);
	if (page_count(page) != expected_count || xas_load(&xas) != page) {
		xas_unlock_irq(&xas);
		return -EAGAIN;
	}

	if (!page_ref_freeze(page, expected_count)) {
		xas_unlock_irq(&xas);
		return -EAGAIN;
	}

	newpage->index = page->index;
	newpage->mapping = page->mapping;

	get_page(newpage);

	xas_store(&xas, newpage);

	page_ref_unfreeze(page, expected_count - 1);

	xas_unlock_irq(&xas);

	return MIGRATEPAGE_SUCCESS;
}

/*
 * Gigantic pages are so large that we do not guarantee that page++ pointer
 * arithmetic will work across the entire page.  We need something more
 * specialized.
 */
static void __copy_gigantic_page(struct page *dst, struct page *src,
				int nr_pages, enum migrate_mode mode)
{
	int i;
	struct page *dst_base = dst;
	struct page *src_base = src;
	int rc = -EFAULT;

	for (i = 0; i < nr_pages; ) {
		cond_resched();

		if (mode & MIGRATE_DMA)
			rc = copy_page_dma(dst, src, 1);

		if (rc)
			copy_highpage(dst, src);

		i++;
		dst = mem_map_next(dst, dst_base, i);
		src = mem_map_next(src, src_base, i);
	}
}

noinline static void copy_huge_page(struct page *dst, struct page *src,
				enum migrate_mode mode)
{
	int i;
	int nr_pages;
	int rc = -EFAULT;

	if (PageHuge(src)) {
		/* hugetlbfs page */
		struct hstate *h = page_hstate(src);
		nr_pages = pages_per_huge_page(h);

		if (unlikely(nr_pages > MAX_ORDER_NR_PAGES)) {
			__copy_gigantic_page(dst, src, nr_pages, mode);
			return;
		}
	} else {
		/* thp page */
		BUG_ON(!PageTransHuge(src));
		nr_pages = hpage_nr_pages(src);
	}

	/* Try to accelerate page migration if it is not specified in mode  */
	// In case of non-concurrent native 2MB page migration RPDAA is used 
	// only when we use multithreaded page copy.
	// Note that the actual number of threads does not have to be more than one.
	// RPDAA works irrespective of actual number of threads but we just need to set 
	// MIGRATE_MT bit in mode.
	if (accel_page_copy || sysctl_enable_page_migration_optimization_avoid_remote_pmem_write==1)
		mode |= MIGRATE_MT;

	if (mode & MIGRATE_MT)
		rc = copy_page_multithread(dst, src, nr_pages);
	else if (mode & MIGRATE_DMA)
		rc = copy_page_dma(dst, src, nr_pages);

	if (rc)
		for (i = 0; i < nr_pages; i++) {
			cond_resched();
			copy_highpage(dst + i, src + i);
		}
}

/*
 * Copy the page to its new location
 */
void migrate_page_states(struct page *newpage, struct page *page)
{
	int cpupid;

	if (PageError(page))
		SetPageError(newpage);
	if (PageReferenced(page))
		SetPageReferenced(newpage);
	if (PageUptodate(page))
		SetPageUptodate(newpage);
	if (TestClearPageActive(page)) {
		VM_BUG_ON_PAGE(PageUnevictable(page), page);
		SetPageActive(newpage);
	} else if (TestClearPageUnevictable(page))
		SetPageUnevictable(newpage);
	if (PageWorkingset(page))
		SetPageWorkingset(newpage);
	if (PageChecked(page))
		SetPageChecked(newpage);
	if (PageMappedToDisk(page))
		SetPageMappedToDisk(newpage);

	/* Move dirty on pages not done by migrate_page_move_mapping() */
	if (PageDirty(page))
		SetPageDirty(newpage);

	if (page_is_young(page))
		set_page_young(newpage);
	if (page_is_idle(page))
		set_page_idle(newpage);

	/*
	 * Copy NUMA information to the new page, to prevent over-eager
	 * future migrations of this same page.
	 */
	cpupid = page_cpupid_xchg_last(page, -1);
	page_cpupid_xchg_last(newpage, cpupid);

	ksm_migrate_page(newpage, page);
	/*
	 * Please do not reorder this without considering how mm/ksm.c's
	 * get_ksm_page() depends upon ksm_migrate_page() and PageSwapCache().
	 */
	if (PageSwapCache(page))
		ClearPageSwapCache(page);
	ClearPagePrivate(page);
	set_page_private(page, 0);

	/*
	 * If any waiters have accumulated on the new page then
	 * wake them up.
	 */
	if (PageWriteback(newpage))
		end_page_writeback(newpage);

	copy_page_owner(page, newpage);

	mem_cgroup_migrate(page, newpage);
}
EXPORT_SYMBOL(migrate_page_states);

void migrate_page_copy(struct page *newpage, struct page *page,
		enum migrate_mode mode)
{
	int rc = -EFAULT;

	if (PageHuge(page) || PageTransHuge(page))
		copy_huge_page(newpage, page, mode);
	else {
		if (mode & MIGRATE_DMA)
			rc = copy_page_dma(newpage, page, 1);
		else if (mode & MIGRATE_MT)
			rc = copy_page_multithread(newpage, page, 1);

		if (rc)
			copy_highpage(newpage, page);
	}

	migrate_page_states(newpage, page);
}
EXPORT_SYMBOL(migrate_page_copy);

/************************************************************
 *                    Migration functions
 ***********************************************************/

/*
 * Common logic to directly migrate a single LRU page suitable for
 * pages that do not use PagePrivate/PagePrivate2.
 *
 * Pages are locked upon entry and exit.
 */
int migrate_page(struct address_space *mapping,
		struct page *newpage, struct page *page,
		enum migrate_mode mode)
{
	int rc;
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	u64 timestamp;
#endif

	BUG_ON(PageWriteback(page));	/* Writeback must be complete */

	rc = migrate_page_move_mapping(mapping, newpage, page, 0);

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.change_page_mapping_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	if (rc != MIGRATEPAGE_SUCCESS)
		return rc;

	if (!(mode & MIGRATE_SYNC_NO_COPY))
		migrate_page_copy(newpage, page, mode);
	else
		migrate_page_states(newpage, page);

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.copy_page_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	return MIGRATEPAGE_SUCCESS;
}
EXPORT_SYMBOL(migrate_page);

#ifdef CONFIG_BLOCK
/* Returns true if all buffers are successfully locked */
bool buffer_migrate_lock_buffers(struct buffer_head *head,
							enum migrate_mode mode)
{
	struct buffer_head *bh = head;

	/* Simple case, sync compaction */
	if ((mode & MIGRATE_MODE_MASK)!= MIGRATE_ASYNC) {
		do {
			lock_buffer(bh);
			bh = bh->b_this_page;

		} while (bh != head);

		return true;
	}

	/* async case, we cannot block on lock_buffer so use trylock_buffer */
	do {
		if (!trylock_buffer(bh)) {
			/*
			 * We failed to lock the buffer and cannot stall in
			 * async migration. Release the taken locks
			 */
			struct buffer_head *failed_bh = bh;
			bh = head;
			while (bh != failed_bh) {
				unlock_buffer(bh);
				bh = bh->b_this_page;
			}
			return false;
		}

		bh = bh->b_this_page;
	} while (bh != head);
	return true;
}

static int __buffer_migrate_page(struct address_space *mapping,
		struct page *newpage, struct page *page, enum migrate_mode mode,
		bool check_refs)
{
	struct buffer_head *bh, *head;
	int rc;
	int expected_count;

	if (!page_has_buffers(page))
		return migrate_page(mapping, newpage, page, mode);

	/* Check whether page does not have extra refs before we do more work */
	expected_count = expected_page_refs(mapping, page);
	if (page_count(page) != expected_count)
		return -EAGAIN;

	head = page_buffers(page);
	if (!buffer_migrate_lock_buffers(head, mode))
		return -EAGAIN;

	if (check_refs) {
		bool busy;
		bool invalidated = false;

recheck_buffers:
		busy = false;
		spin_lock(&mapping->private_lock);
		bh = head;
		do {
			if (atomic_read(&bh->b_count)) {
				busy = true;
				break;
			}
			bh = bh->b_this_page;
		} while (bh != head);
		if (busy) {
			if (invalidated) {
				rc = -EAGAIN;
				goto unlock_buffers;
			}
			spin_unlock(&mapping->private_lock);
			invalidate_bh_lrus();
			invalidated = true;
			goto recheck_buffers;
		}
	}

	rc = migrate_page_move_mapping(mapping, newpage, page, 0);
	if (rc != MIGRATEPAGE_SUCCESS)
		goto unlock_buffers;

	ClearPagePrivate(page);
	set_page_private(newpage, page_private(page));
	set_page_private(page, 0);
	put_page(page);
	get_page(newpage);

	bh = head;
	do {
		set_bh_page(bh, newpage, bh_offset(bh));
		bh = bh->b_this_page;

	} while (bh != head);

	SetPagePrivate(newpage);

	if (!(mode & MIGRATE_SYNC_NO_COPY))
		migrate_page_copy(newpage, page, MIGRATE_SINGLETHREAD);
	else
		migrate_page_states(newpage, page);

	rc = MIGRATEPAGE_SUCCESS;
unlock_buffers:
	if (check_refs)
		spin_unlock(&mapping->private_lock);
	bh = head;
	do {
		unlock_buffer(bh);
		bh = bh->b_this_page;

	} while (bh != head);

	return rc;
}

/*
 * Migration function for pages with buffers. This function can only be used
 * if the underlying filesystem guarantees that no other references to "page"
 * exist. For example attached buffer heads are accessed only under page lock.
 */
int buffer_migrate_page(struct address_space *mapping,
		struct page *newpage, struct page *page, enum migrate_mode mode)
{
	return __buffer_migrate_page(mapping, newpage, page, mode, false);
}
EXPORT_SYMBOL(buffer_migrate_page);

/*
 * Same as above except that this variant is more careful and checks that there
 * are also no buffer head references. This function is the right one for
 * mappings where buffer heads are directly looked up and referenced (such as
 * block device mappings).
 */
int buffer_migrate_page_norefs(struct address_space *mapping,
		struct page *newpage, struct page *page, enum migrate_mode mode)
{
	return __buffer_migrate_page(mapping, newpage, page, mode, true);
}
#endif

/*
 * Writeback a page to clean the dirty state
 */
int writeout(struct address_space *mapping, struct page *page)
{
	struct writeback_control wbc = {
		.sync_mode = WB_SYNC_NONE,
		.nr_to_write = 1,
		.range_start = 0,
		.range_end = LLONG_MAX,
		.for_reclaim = 1
	};
	int rc;

	if (!mapping->a_ops->writepage)
		/* No write method for the address space */
		return -EINVAL;

	if (!clear_page_dirty_for_io(page))
		/* Someone else already triggered a write */
		return -EAGAIN;

	/*
	 * A dirty page may imply that the underlying filesystem has
	 * the page on some queue. So the page must be clean for
	 * migration. Writeout may mean we loose the lock and the
	 * page state is no longer what we checked for earlier.
	 * At this point we know that the migration attempt cannot
	 * be successful.
	 */
	remove_migration_ptes(page, page, false);

	rc = mapping->a_ops->writepage(page, &wbc);

	if (rc != AOP_WRITEPAGE_ACTIVATE)
		/* unlocked. Relock */
		lock_page(page);

	return (rc < 0) ? -EIO : -EAGAIN;
}

/*
 * Default handling if a filesystem does not provide a migration function.
 */
static int fallback_migrate_page(struct address_space *mapping,
	struct page *newpage, struct page *page, enum migrate_mode mode)
{
	if (PageDirty(page)) {
		/* Only writeback pages in full synchronous migration */
		if ((mode & MIGRATE_MODE_MASK) != MIGRATE_SYNC)
			return -EBUSY;
		return writeout(mapping, page);
	}

	/*
	 * Buffers may be managed in a filesystem specific way.
	 * We must have no buffers or drop them.
	 */
	if (page_has_private(page) &&
	    !try_to_release_page(page, GFP_KERNEL))
		return (mode & MIGRATE_MODE_MASK) == MIGRATE_SYNC ? -EAGAIN : -EBUSY;

	return migrate_page(mapping, newpage, page, mode);
}

/*
 * Move a page to a newly allocated page
 * The page is locked and all ptes have been successfully removed.
 *
 * The new page will have replaced the old page if this function
 * is successful.
 *
 * Return value:
 *   < 0 - error code
 *  MIGRATEPAGE_SUCCESS - success
 */
static int move_to_new_page(struct page *newpage, struct page *page,
				enum migrate_mode mode)
{
	struct address_space *mapping;
	int rc = -EAGAIN;
	bool is_lru = !__PageMovable(page);

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(!PageLocked(newpage), newpage);

	mapping = page_mapping(page);

	if (likely(is_lru)) {
		if (!mapping)
			rc = migrate_page(mapping, newpage, page, mode);
		else if (mapping->a_ops->migratepage)
			/*
			 * Most pages have a mapping and most filesystems
			 * provide a migratepage callback. Anonymous pages
			 * are part of swap space which also has its own
			 * migratepage callback. This is the most common path
			 * for page migration.
			 */
			rc = mapping->a_ops->migratepage(mapping, newpage,
							page, mode);
		else
			rc = fallback_migrate_page(mapping, newpage,
							page, mode);
	} else {
		/*
		 * In case of non-lru page, it could be released after
		 * isolation step. In that case, we shouldn't try migration.
		 */
		VM_BUG_ON_PAGE(!PageIsolated(page), page);
		if (!PageMovable(page)) {
			rc = MIGRATEPAGE_SUCCESS;
			__ClearPageIsolated(page);
			goto out;
		}

		rc = mapping->a_ops->migratepage(mapping, newpage,
						page, mode);
		WARN_ON_ONCE(rc == MIGRATEPAGE_SUCCESS &&
			!PageIsolated(page));
	}

	/*
	 * When successful, old pagecache page->mapping must be cleared before
	 * page is freed; but stats require that PageAnon be left as PageAnon.
	 */
	if (rc == MIGRATEPAGE_SUCCESS) {
		if (__PageMovable(page)) {
			VM_BUG_ON_PAGE(!PageIsolated(page), page);

			/*
			 * We clear PG_movable under page_lock so any compactor
			 * cannot try to migrate this page.
			 */
			__ClearPageIsolated(page);
		}

		/*
		 * Anonymous and movable page->mapping will be cleared by
		 * free_pages_prepare so don't reset it here for keeping
		 * the type to work PageAnon, for example.
		 */
		if (!PageMappingFlags(page))
			page->mapping = NULL;

		if (likely(!is_zone_device_page(newpage)))
			flush_dcache_page(newpage);

	}
out:
	return rc;
}

static int __unmap_and_move(struct page *page, struct page *newpage,
				int force, enum migrate_mode mode)
{
	int rc = -EAGAIN;
	int page_was_mapped = 0;
	struct anon_vma *anon_vma = NULL;
	bool is_lru = !__PageMovable(page);
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	u64 timestamp;
#endif

	if (!trylock_page(page)) {
		if (!force || ((mode & MIGRATE_MODE_MASK) == MIGRATE_ASYNC))
			goto out;

		/*
		 * It's not safe for direct compaction to call lock_page.
		 * For example, during page readahead pages are added locked
		 * to the LRU. Later, when the IO completes the pages are
		 * marked uptodate and unlocked. However, the queueing
		 * could be merging multiple pages for one bio (e.g.
		 * mpage_readpages). If an allocation happens for the
		 * second or third page, the process can end up locking
		 * the same page twice and deadlocking. Rather than
		 * trying to be clever about what pages can be locked,
		 * avoid the use of lock_page for direct compaction
		 * altogether.
		 */
		if (current->flags & PF_MEMALLOC)
			goto out;

		lock_page(page);
	}

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.lock_page_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	if (PageWriteback(page)) {
		/*
		 * Only in the case of a full synchronous migration is it
		 * necessary to wait for PageWriteback. In the async case,
		 * the retry loop is too short and in the sync-light case,
		 * the overhead of stalling is too much
		 */
		if ((mode & MIGRATE_MODE_MASK) != MIGRATE_SYNC) {
			rc = -EBUSY;
			goto out_unlock;
		}
		if (!force)
			goto out_unlock;
		wait_on_page_writeback(page);
	}

	/*
	 * By try_to_unmap(), page->mapcount goes down to 0 here. In this case,
	 * we cannot notice that anon_vma is freed while we migrates a page.
	 * This get_anon_vma() delays freeing anon_vma pointer until the end
	 * of migration. File cache pages are no problem because of page_lock()
	 * File Caches may use write_page() or lock_page() in migration, then,
	 * just care Anon page here.
	 *
	 * Only page_get_anon_vma() understands the subtleties of
	 * getting a hold on an anon_vma from outside one of its mms.
	 * But if we cannot get anon_vma, then we won't need it anyway,
	 * because that implies that the anon page is no longer mapped
	 * (and cannot be remapped so long as we hold the page lock).
	 */
	if (PageAnon(page) && !PageKsm(page))
		anon_vma = page_get_anon_vma(page);

	/*
	 * Block others from accessing the new page when we get around to
	 * establishing additional references. We are usually the only one
	 * holding a reference to newpage at this point. We used to have a BUG
	 * here if trylock_page(newpage) fails, but would like to allow for
	 * cases where there might be a race with the previous use of newpage.
	 * This is much like races on refcount of oldpage: just don't BUG().
	 */
	if (unlikely(!trylock_page(newpage)))
		goto out_unlock;

	if (unlikely(!is_lru)) {
		rc = move_to_new_page(newpage, page, mode);
		goto out_unlock_both;
	}

	/*
	 * Corner case handling:
	 * 1. When a new swap-cache page is read into, it is added to the LRU
	 * and treated as swapcache but it has no rmap yet.
	 * Calling try_to_unmap() against a page->mapping==NULL page will
	 * trigger a BUG.  So handle it here.
	 * 2. An orphaned page (see truncate_complete_page) might have
	 * fs-private metadata. The page can be picked up due to memory
	 * offlining.  Everywhere else except page reclaim, the page is
	 * invisible to the vm, so the page can not be migrated.  So try to
	 * free the metadata, so the page can be freed.
	 */
	if (!page->mapping) {
		VM_BUG_ON_PAGE(PageAnon(page), page);
		if (page_has_private(page)) {
			try_to_free_buffers(page);
			goto out_unlock_both;
		}
	} else if (page_mapped(page)) {
		/* Establish migration ptes */
		VM_BUG_ON_PAGE(PageAnon(page) && !PageKsm(page) && !anon_vma,
				page);
		try_to_unmap(page,
			TTU_MIGRATION|TTU_IGNORE_MLOCK|TTU_IGNORE_ACCESS);
		page_was_mapped = 1;
	}

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.unmap_page_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	if (!page_mapped(page))
		rc = move_to_new_page(newpage, page, mode);

	if (page_was_mapped)
		remove_migration_ptes(page,
			rc == MIGRATEPAGE_SUCCESS ? newpage : page, false);

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.remove_migration_ptes_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

out_unlock_both:
	unlock_page(newpage);
out_unlock:
	/* Drop an anon_vma reference if we took one */
	if (anon_vma)
		put_anon_vma(anon_vma);
	unlock_page(page);
out:
	/*
	 * If migration is successful, decrease refcount of the newpage
	 * which will not free the page because new page owner increased
	 * refcounter. As well, if it is LRU page, add the page to LRU
	 * list in here. Use the old state of the isolated source page to
	 * determine if we migrated a LRU page. newpage was already unlocked
	 * and possibly modified by its owner - don't rely on the page
	 * state.
	 */
	if (rc == MIGRATEPAGE_SUCCESS) {
		if (unlikely(!is_lru))
			put_page(newpage);
		else
			putback_lru_page(newpage);
	}

	return rc;
}

/*
 * gcc 4.7 and 4.8 on arm get an ICEs when inlining unmap_and_move().  Work
 * around it.
 */
#if defined(CONFIG_ARM) && \
	defined(GCC_VERSION) && GCC_VERSION < 40900 && GCC_VERSION >= 40700
#define ICE_noinline noinline
#else
#define ICE_noinline
#endif

/*
 * Obtain the lock on page, remove all ptes and migrate the page
 * to the newly allocated page in newpage.
 */
static ICE_noinline int unmap_and_move(new_page_t get_new_page,
				   free_page_t put_new_page,
				   unsigned long private, struct page *page,
				   int force, enum migrate_mode mode,
				   enum migrate_reason reason)
{
	int rc = MIGRATEPAGE_SUCCESS;
	struct page *newpage = NULL;
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	u64 timestamp;
#endif

	if (!thp_migration_supported() && PageTransHuge(page))
		return -ENOMEM;

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.enter_unmap_and_move_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.get_new_page_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	if (page_count(page) == 1) {
		/* page was freed from under us. So we are done. */
		ClearPageActive(page);
		ClearPageUnevictable(page);
		if (unlikely(__PageMovable(page))) {
			lock_page(page);
			if (!PageMovable(page))
				__ClearPageIsolated(page);
			unlock_page(page);
		}

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.putback_old_page_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.putback_new_page_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif

		goto out;
	}

	newpage = get_new_page(page, private);
	if (!newpage)
		return -ENOMEM;

	rc = __unmap_and_move(page, newpage, force, mode);
	if (rc == MIGRATEPAGE_SUCCESS)
		set_page_owner_migrate_reason(newpage, reason);

out:
	if (rc != -EAGAIN) {
		/*
		 * A page that has been migrated has all references
		 * removed and will be freed. A page that has not been
		 * migrated will have kept its references and be restored.
		 */
		list_del(&page->lru);

		/*
		 * Compaction can migrate also non-LRU pages which are
		 * not accounted to NR_ISOLATED_*. They can be recognized
		 * as __PageMovable
		 */
		if (likely(!__PageMovable(page)))
			mod_node_page_state(page_pgdat(page), NR_ISOLATED_ANON +
					page_is_file_cache(page), -hpage_nr_pages(page));
	}

	/*
	 * If migration is successful, releases reference grabbed during
	 * isolation. Otherwise, restore the page to right list unless
	 * we want to retry.
	 */
	if (rc == MIGRATEPAGE_SUCCESS) {
		put_page(page);
		if (reason == MR_MEMORY_FAILURE) {
			/*
			 * Set PG_HWPoison on just freed page
			 * intentionally. Although it's rather weird,
			 * it's how HWPoison flag works at the moment.
			 */
			if (set_hwpoison_free_buddy_page(page))
				num_poisoned_pages_inc();
		}

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.putback_old_page_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	} else {
		if (rc != -EAGAIN) {
			if (likely(!__PageMovable(page))) {
				putback_lru_page(page);
				goto put_new;
			}

			lock_page(page);
			if (PageMovable(page))
				putback_movable_page(page);
			else
				__ClearPageIsolated(page);
			unlock_page(page);
			put_page(page);
		}

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.putback_old_page_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif

put_new:
		if (put_new_page)
			put_new_page(newpage, private);
		else
			put_page(newpage);

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.putback_new_page_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	}

	return rc;
}

/*
 * Counterpart of unmap_and_move_page() for hugepage migration.
 *
 * This function doesn't wait the completion of hugepage I/O
 * because there is no race between I/O and migration for hugepage.
 * Note that currently hugepage I/O occurs only in direct I/O
 * where no lock is held and PG_writeback is irrelevant,
 * and writeback status of all subpages are counted in the reference
 * count of the head page (i.e. if all subpages of a 2MB hugepage are
 * under direct I/O, the reference of the head page is 512 and a bit more.)
 * This means that when we try to migrate hugepage whose subpages are
 * doing direct I/O, some references remain after try_to_unmap() and
 * hugepage migration fails without data corruption.
 *
 * There is also no race when direct I/O is issued on the page under migration,
 * because then pte is replaced with migration swap entry and direct I/O code
 * will wait in the page fault for migration to complete.
 */
static int unmap_and_move_huge_page(new_page_t get_new_page,
				free_page_t put_new_page, unsigned long private,
				struct page *hpage, int force,
				enum migrate_mode mode, int reason)
{
	int rc = -EAGAIN;
	int page_was_mapped = 0;
	struct page *new_hpage;
	struct anon_vma *anon_vma = NULL;

	/*
	 * Migratability of hugepages depends on architectures and their size.
	 * This check is necessary because some callers of hugepage migration
	 * like soft offline and memory hotremove don't walk through page
	 * tables or check whether the hugepage is pmd-based or not before
	 * kicking migration.
	 */
	if (!hugepage_migration_supported(page_hstate(hpage))) {
		putback_active_hugepage(hpage);
		return -ENOSYS;
	}

	new_hpage = get_new_page(hpage, private);
	if (!new_hpage)
		return -ENOMEM;

	if (!trylock_page(hpage)) {
		if (!force || ((mode & MIGRATE_MODE_MASK) != MIGRATE_SYNC))
			goto out;
		lock_page(hpage);
	}

	/*
	 * Check for pages which are in the process of being freed.  Without
	 * page_mapping() set, hugetlbfs specific move page routine will not
	 * be called and we could leak usage counts for subpools.
	 */
	if (page_private(hpage) && !page_mapping(hpage)) {
		rc = -EBUSY;
		goto out_unlock;
	}

	if (PageAnon(hpage))
		anon_vma = page_get_anon_vma(hpage);

	if (unlikely(!trylock_page(new_hpage)))
		goto put_anon;

	if (page_mapped(hpage)) {
		try_to_unmap(hpage,
			TTU_MIGRATION|TTU_IGNORE_MLOCK|TTU_IGNORE_ACCESS);
		page_was_mapped = 1;
	}

	if (!page_mapped(hpage))
		rc = move_to_new_page(new_hpage, hpage, mode);

	if (page_was_mapped)
		remove_migration_ptes(hpage,
			rc == MIGRATEPAGE_SUCCESS ? new_hpage : hpage, false);

	unlock_page(new_hpage);

put_anon:
	if (anon_vma)
		put_anon_vma(anon_vma);

	if (rc == MIGRATEPAGE_SUCCESS) {
		move_hugetlb_state(hpage, new_hpage, reason);
		put_new_page = NULL;
	}

out_unlock:
	unlock_page(hpage);
out:
	if (rc != -EAGAIN)
		putback_active_hugepage(hpage);

	/*
	 * If migration was not successful and there's a freeing callback, use
	 * it.  Otherwise, put_page() will drop the reference grabbed during
	 * isolation.
	 */
	if (put_new_page)
		put_new_page(new_hpage, private);
	else
		putback_active_hugepage(new_hpage);

	return rc;
}

static int __unmap_page_concur(struct page *page, struct page *newpage,
				struct anon_vma **anon_vma,
				int *page_was_mapped,
				int force, enum migrate_mode mode)
{
	int rc = -EAGAIN;
	bool is_lru = !__PageMovable(page);
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	u64 timestamp;
#endif

	*anon_vma = NULL;
	*page_was_mapped = 0;

	if (!trylock_page(page)) {
		if (!force || ((mode & MIGRATE_MODE_MASK) == MIGRATE_ASYNC))
			goto out;

		/*
		 * It's not safe for direct compaction to call lock_page.
		 * For example, during page readahead pages are added locked
		 * to the LRU. Later, when the IO completes the pages are
		 * marked uptodate and unlocked. However, the queueing
		 * could be merging multiple pages for one bio (e.g.
		 * mpage_readpages). If an allocation happens for the
		 * second or third page, the process can end up locking
		 * the same page twice and deadlocking. Rather than
		 * trying to be clever about what pages can be locked,
		 * avoid the use of lock_page for direct compaction
		 * altogether.
		 */
		if (current->flags & PF_MEMALLOC)
			goto out;

		lock_page(page);
	}

	/* We are working on page_mapping(page) == NULL */
	VM_BUG_ON_PAGE(PageWriteback(page), page);
#if 0
	if (PageWriteback(page)) {
		/*
		 * Only in the case of a full synchronous migration is it
		 * necessary to wait for PageWriteback. In the async case,
		 * the retry loop is too short and in the sync-light case,
		 * the overhead of stalling is too much
		 */
		if ((mode & MIGRATE_MODE_MASK) != MIGRATE_SYNC) {
			rc = -EBUSY;
			goto out_unlock;
		}
		if (!force)
			goto out_unlock;
		wait_on_page_writeback(page);
	}
#endif

	/*
	 * By try_to_unmap(), page->mapcount goes down to 0 here. In this case,
	 * we cannot notice that anon_vma is freed while we migrates a page.
	 * This get_anon_vma() delays freeing anon_vma pointer until the end
	 * of migration. File cache pages are no problem because of page_lock()
	 * File Caches may use write_page() or lock_page() in migration, then,
	 * just care Anon page here.
	 *
	 * Only page_get_anon_vma() understands the subtleties of
	 * getting a hold on an anon_vma from outside one of its mms.
	 * But if we cannot get anon_vma, then we won't need it anyway,
	 * because that implies that the anon page is no longer mapped
	 * (and cannot be remapped so long as we hold the page lock).
	 */
	if (PageAnon(page) && !PageKsm(page))
		*anon_vma = page_get_anon_vma(page);

	/*
	 * Block others from accessing the new page when we get around to
	 * establishing additional references. We are usually the only one
	 * holding a reference to newpage at this point. We used to have a BUG
	 * here if trylock_page(newpage) fails, but would like to allow for
	 * cases where there might be a race with the previous use of newpage.
	 * This is much like races on refcount of oldpage: just don't BUG().
	 */
	if (unlikely(!trylock_page(newpage)))
		goto out_unlock;

	if (unlikely(!is_lru)) {
		/* Just migrate the page and remove it from item list */
		VM_BUG_ON(1);
		rc = move_to_new_page(newpage, page, mode);
		goto out_unlock_both;
	}

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.lock_page_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	/*
	 * Corner case handling:
	 * 1. When a new swap-cache page is read into, it is added to the LRU
	 * and treated as swapcache but it has no rmap yet.
	 * Calling try_to_unmap() against a page->mapping==NULL page will
	 * trigger a BUG.  So handle it here.
	 * 2. An orphaned page (see truncate_complete_page) might have
	 * fs-private metadata. The page can be picked up due to memory
	 * offlining.  Everywhere else except page reclaim, the page is
	 * invisible to the vm, so the page can not be migrated.  So try to
	 * free the metadata, so the page can be freed.
	 */
	if (!page->mapping) {
		VM_BUG_ON_PAGE(PageAnon(page), page);
		if (page_has_private(page)) {
			try_to_free_buffers(page);
			goto out_unlock_both;
		}
	} else if (page_mapped(page)) {
		/* Establish migration ptes */
		VM_BUG_ON_PAGE(PageAnon(page) && !PageKsm(page) && !*anon_vma,
				page);
		try_to_unmap(page,
			TTU_MIGRATION|TTU_IGNORE_MLOCK|TTU_IGNORE_ACCESS);
		*page_was_mapped = 1;
	}

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.unmap_page_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	return MIGRATEPAGE_SUCCESS;

out_unlock_both:
	unlock_page(newpage);
out_unlock:
	/* Drop an anon_vma reference if we took one */
	if (*anon_vma)
		put_anon_vma(*anon_vma);
	unlock_page(page);
out:
	return rc;
}

static int unmap_pages_and_get_new_concur(new_page_t get_new_page,
				free_page_t put_new_page, unsigned long private,
				struct page_migration_work_item *item,
				int force,
				enum migrate_mode mode, enum migrate_reason reason)
{
	int rc = MIGRATEPAGE_SUCCESS;
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	u64 timestamp;
#endif

	if (!thp_migration_supported() && PageTransHuge(item->old_page))
		return -ENOMEM;

	item->new_page = get_new_page(item->old_page, private);
	if (!item->new_page)
		return -ENOMEM;

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.get_new_page_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	if (page_count(item->old_page) == 1) {
		/* page was freed from under us. So we are done. */
		ClearPageActive(item->old_page);
		ClearPageUnevictable(item->old_page);
		if (unlikely(__PageMovable(item->old_page))) {
			lock_page(item->old_page);
			if (!PageMovable(item->old_page))
				__ClearPageIsolated(item->old_page);
			unlock_page(item->old_page);
		}

		if (put_new_page)
			put_new_page(item->new_page, private);
		else
			put_page(item->new_page);
		item->new_page = NULL;

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.putback_new_page_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif
		goto out;
	}

	rc = __unmap_page_concur(item->old_page, item->new_page, &item->anon_vma,
							&item->page_was_mapped,
							force, mode);
	if (rc == MIGRATEPAGE_SUCCESS)
		return rc;

out:
	if (rc != -EAGAIN) {
		list_del(&item->old_page->lru);

		if (likely(!__PageMovable(item->old_page)))
			mod_node_page_state(page_pgdat(item->old_page), NR_ISOLATED_ANON +
					page_is_file_cache(item->old_page),
					-hpage_nr_pages(item->old_page));
	}

	if (rc == MIGRATEPAGE_SUCCESS) {
		/* only for pages freed under us  */
		VM_BUG_ON(page_count(item->old_page) != 1);
		put_page(item->old_page);
		item->old_page = NULL;

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.putback_old_page_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif
	} else {
		if (rc != -EAGAIN) {
			if (likely(!__PageMovable(item->old_page))) {
				putback_lru_page(item->old_page);
				goto put_new;
			}

			lock_page(item->old_page);
			if (PageMovable(item->old_page))
				putback_movable_page(item->old_page);
			else
				__ClearPageIsolated(item->old_page);
			unlock_page(item->old_page);
			put_page(item->old_page);
		}

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.putback_old_page_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif

		/*
		 * If migration was not successful and there's a freeing callback, use
		 * it.  Otherwise, putback_lru_page() will drop the reference grabbed
		 * during isolation.
		 */
put_new:
		if (put_new_page)
			put_new_page(item->new_page, private);
		else
			put_page(item->new_page);
		item->new_page = NULL;

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.putback_new_page_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif
	}
	return rc;
}

static int move_mapping_concurr(struct list_head *unmapped_list_ptr,
					   struct list_head *wip_list_ptr,
					   free_page_t put_new_page, unsigned long private,
					   enum migrate_mode mode)
{
	struct page_migration_work_item *iterator, *iterator2;
	struct address_space *mapping;

	list_for_each_entry_safe(iterator, iterator2, unmapped_list_ptr, list) {
		VM_BUG_ON_PAGE(!PageLocked(iterator->old_page), iterator->old_page);
		VM_BUG_ON_PAGE(!PageLocked(iterator->new_page), iterator->new_page);

		mapping = page_mapping(iterator->old_page);

		VM_BUG_ON(mapping);

		VM_BUG_ON(PageWriteback(iterator->old_page));

		if (page_count(iterator->old_page) != 1) {
			list_move(&iterator->list, wip_list_ptr);
			if (iterator->page_was_mapped)
				remove_migration_ptes(iterator->old_page,
					iterator->old_page, false);
			unlock_page(iterator->new_page);
			if (iterator->anon_vma)
				put_anon_vma(iterator->anon_vma);
			unlock_page(iterator->old_page);

			if (put_new_page)
				put_new_page(iterator->new_page, private);
			else
				put_page(iterator->new_page);
			iterator->new_page = NULL;
			continue;
		}

		iterator->new_page->index = iterator->old_page->index;
		iterator->new_page->mapping = iterator->old_page->mapping;
		if (PageSwapBacked(iterator->old_page))
			SetPageSwapBacked(iterator->new_page);
	}

	return 0;
}

static int copy_to_new_pages_concur(struct list_head *unmapped_list_ptr,
				enum migrate_mode mode)
{
	struct page_migration_work_item *iterator;
	int num_pages = 0, idx = 0;
	struct page **src_page_list = NULL, **dst_page_list = NULL;
	unsigned long size = 0;
	int rc = -EFAULT;
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	u64 timestamp;
#endif

	if (list_empty(unmapped_list_ptr))
		return 0;

	list_for_each_entry(iterator, unmapped_list_ptr, list) {
		++num_pages;
		size += PAGE_SIZE * hpage_nr_pages(iterator->old_page);
	}

	src_page_list = kzalloc(sizeof(struct page *)*num_pages, GFP_KERNEL);
	if (!src_page_list) {
		BUG();
		return -ENOMEM;
	}
	dst_page_list = kzalloc(sizeof(struct page *)*num_pages, GFP_KERNEL);
	if (!dst_page_list) {
		BUG();
		return -ENOMEM;
	}

	list_for_each_entry(iterator, unmapped_list_ptr, list) {
		src_page_list[idx] = iterator->old_page;
		dst_page_list[idx] = iterator->new_page;
		++idx;
	}

	BUG_ON(idx != num_pages);

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.change_page_mapping_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	if (mode & MIGRATE_DMA)
		rc = copy_page_lists_dma_always(dst_page_list, src_page_list,
							num_pages);
	else if (mode & MIGRATE_MT)
		rc = copy_page_lists_mt(dst_page_list, src_page_list,
							num_pages);

	if (rc) {
		list_for_each_entry(iterator, unmapped_list_ptr, list) {
			if (PageHuge(iterator->old_page) ||
				PageTransHuge(iterator->old_page))
				copy_huge_page(iterator->new_page, iterator->old_page, 0);
			else
				copy_highpage(iterator->new_page, iterator->old_page);
		}
	}

	list_for_each_entry(iterator, unmapped_list_ptr, list) {
		migrate_page_states(iterator->new_page, iterator->old_page);
	}

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.copy_page_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	kfree(src_page_list);
	kfree(dst_page_list);

	return 0;
}

static int remove_migration_ptes_concurr(struct list_head *unmapped_list_ptr)
{
	struct page_migration_work_item *iterator, *iterator2;
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	u64 timestamp;
#endif

	list_for_each_entry_safe(iterator, iterator2, unmapped_list_ptr, list) {
		if (iterator->page_was_mapped)
			remove_migration_ptes(iterator->old_page, iterator->new_page, false);


#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.remove_migration_ptes_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif

		unlock_page(iterator->new_page);

		if (iterator->anon_vma)
			put_anon_vma(iterator->anon_vma);

		unlock_page(iterator->old_page);

		list_del(&iterator->old_page->lru);
		mod_node_page_state(page_pgdat(iterator->old_page), NR_ISOLATED_ANON +
				page_is_file_cache(iterator->old_page),
				-hpage_nr_pages(iterator->old_page));

		put_page(iterator->old_page);
		iterator->old_page = NULL;

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.putback_old_page_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif

		if (unlikely(__PageMovable(iterator->new_page)))
			put_page(iterator->new_page);
		else
			putback_lru_page(iterator->new_page);
		iterator->new_page = NULL;

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.putback_new_page_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif
	}

	return 0;
}

int migrate_pages_concur(struct list_head *from, new_page_t get_new_page,
		free_page_t put_new_page, unsigned long private,
		enum migrate_mode mode, int reason)
{
	int retry = 1;
	int nr_failed = 0;
	int nr_succeeded = 0;
	int pass = 0;
	struct page *page;
	int swapwrite = current->flags & PF_SWAPWRITE;
	int rc;
	int total_num_pages = 0, idx;
	struct page_migration_work_item *item_list;
	struct page_migration_work_item *iterator, *iterator2;
	int item_list_order = 0;
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	u64 timestamp;
#endif

	LIST_HEAD(wip_list);
	LIST_HEAD(unmapped_list);
	LIST_HEAD(serialized_list);
	LIST_HEAD(failed_list);

	if (!swapwrite)
		current->flags |= PF_SWAPWRITE;

	list_for_each_entry(page, from, lru)
		++total_num_pages;

	item_list_order = get_order(total_num_pages *
		sizeof(struct page_migration_work_item));

	if (item_list_order > MAX_ORDER) {
		item_list = alloc_pages_exact(total_num_pages *
			sizeof(struct page_migration_work_item), GFP_ATOMIC);
		memset(item_list, 0, total_num_pages *
			sizeof(struct page_migration_work_item));
	} else {
		item_list = (struct page_migration_work_item *)__get_free_pages(GFP_ATOMIC,
						item_list_order);
		memset(item_list, 0, PAGE_SIZE<<item_list_order);
	}

	idx = 0;
	list_for_each_entry(page, from, lru) {
		item_list[idx].old_page = page;
		item_list[idx].new_page = NULL;
		INIT_LIST_HEAD(&item_list[idx].list);
		list_add_tail(&item_list[idx].list, &wip_list);
		idx += 1;
	}

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.enter_unmap_and_move_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	for(pass = 0; pass < 1 && retry; pass++) {
		retry = 0;

		/* unmap and get new page for page_mapping(page) == NULL */
		list_for_each_entry_safe(iterator, iterator2, &wip_list, list) {
			cond_resched();

			if (iterator->new_page) {
				pr_info("%s: iterator already has a new page?\n", __func__);
				VM_BUG_ON_PAGE(1, iterator->old_page);
			}

			/* We do not migrate huge pages, file-backed, or swapcached pages */
			if (PageHuge(iterator->old_page)) {
				rc = -ENODEV;
			}
			else if ((page_mapping(iterator->old_page) != NULL)) {
				rc = -ENODEV;
			}
			else
				rc = unmap_pages_and_get_new_concur(get_new_page, put_new_page,
						private, iterator, pass > 2, mode,
						reason);

			switch(rc) {
			case -ENODEV:
				list_move(&iterator->list, &serialized_list);
				break;
			case -ENOMEM:
				if (PageTransHuge(page))
					list_move(&iterator->list, &serialized_list);
				else
					goto out;
				break;
			case -EAGAIN:
				retry++;
				break;
			case MIGRATEPAGE_SUCCESS:
				if (iterator->old_page) {
					list_move(&iterator->list, &unmapped_list);
					nr_succeeded++;
				} else { /* pages are freed under us */
					list_del(&iterator->list);
				}
				break;
			default:
				/*
				 * Permanent failure (-EBUSY, -ENOSYS, etc.):
				 * unlike -EAGAIN case, the failed page is
				 * removed from migration page list and not
				 * retried in the next outer loop.
				 */
				list_move(&iterator->list, &failed_list);
				nr_failed++;
				break;
			}
		}
out:
		if (list_empty(&unmapped_list))
			continue;

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.unmap_page_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif

		/* move page->mapping to new page, only -EAGAIN could happen  */
		move_mapping_concurr(&unmapped_list, &wip_list, put_new_page, private, mode);

		/* copy pages in unmapped_list */
		copy_to_new_pages_concur(&unmapped_list, mode);

		/* remove migration pte, if old_page is NULL?, unlock old and new
		 * pages, put anon_vma, put old and new pages */
		remove_migration_ptes_concurr(&unmapped_list);

	}
	nr_failed += retry;
	rc = nr_failed;

	if (!list_empty(from))
		rc = migrate_pages(from, get_new_page, put_new_page,
				private, mode, reason);

	if (nr_succeeded)
		count_vm_events(PGMIGRATE_SUCCESS, nr_succeeded);
	if (nr_failed)
		count_vm_events(PGMIGRATE_FAIL, nr_failed);
	trace_mm_migrate_pages(nr_succeeded, nr_failed, mode, reason);

	if (item_list_order >= MAX_ORDER) {
		free_pages_exact(item_list, total_num_pages *
			sizeof(struct page_migration_work_item));
	} else {
		free_pages((unsigned long)item_list, item_list_order);
	}

	if (!swapwrite)
		current->flags &= ~PF_SWAPWRITE;

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.migrate_pages_cleanup_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	return rc;
}

/*
 * migrate_pages - migrate the pages specified in a list, to the free pages
 *		   supplied as the target for the page migration
 *
 * @from:		The list of pages to be migrated.
 * @get_new_page:	The function used to allocate free pages to be used
 *			as the target of the page migration.
 * @put_new_page:	The function used to free target pages if migration
 *			fails, or NULL if no special handling is necessary.
 * @private:		Private data to be passed on to get_new_page()
 * @mode:		The migration mode that specifies the constraints for
 *			page migration, if any.
 * @reason:		The reason for page migration.
 *
 * The function returns after 10 attempts or if no pages are movable any more
 * because the list has become empty or no retryable pages exist any more.
 * The caller should call putback_movable_pages() to return pages to the LRU
 * or free list only if ret != 0.
 *
 * Returns the number of pages that were not migrated, or an error code.
 */
int migrate_pages(struct list_head *from, new_page_t get_new_page,
		free_page_t put_new_page, unsigned long private,
		enum migrate_mode mode, int reason)
{
	int retry = 1;
	int nr_failed = 0;
	int nr_succeeded = 0;
	int pass = 0;
	struct page *page;
	struct page *page2;
	int swapwrite = current->flags & PF_SWAPWRITE;
	int rc;
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	u64 timestamp;
#endif

	if (!swapwrite)
		current->flags |= PF_SWAPWRITE;

	for(pass = 0; pass < 10 && retry; pass++) {
		retry = 0;

		list_for_each_entry_safe(page, page2, from, lru) {
retry:
			cond_resched();

			if (PageHuge(page))
				rc = unmap_and_move_huge_page(get_new_page,
						put_new_page, private, page,
						pass > 2, mode, reason);
			else
				rc = unmap_and_move(get_new_page, put_new_page,
						private, page, pass > 2, mode,
						reason);

			switch(rc) {
			case -ENOMEM:
				/*
				 * THP migration might be unsupported or the
				 * allocation could've failed so we should
				 * retry on the same page with the THP split
				 * to base pages.
				 *
				 * Head page is retried immediately and tail
				 * pages are added to the tail of the list so
				 * we encounter them after the rest of the list
				 * is processed.
				 */
				if (PageTransHuge(page) && !PageHuge(page)) {
					lock_page(page);
					rc = split_huge_page_to_list(page, from);
					unlock_page(page);

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
					timestamp = rdtsc();
					current->move_pages_breakdown.split_thp_page_cycles += timestamp -
						current->move_pages_breakdown.last_timestamp;
					current->move_pages_breakdown.last_timestamp = timestamp;
#endif

					if (!rc) {
						list_safe_reset_next(page, page2, lru);
						goto retry;
					}
				}
				nr_failed++;
				goto out;
			case -EAGAIN:
				retry++;
				break;
			case MIGRATEPAGE_SUCCESS:
				nr_succeeded++;
				break;
			default:
				/*
				 * Permanent failure (-EBUSY, -ENOSYS, etc.):
				 * unlike -EAGAIN case, the failed page is
				 * removed from migration page list and not
				 * retried in the next outer loop.
				 */
				nr_failed++;
				break;
			}
		}
	}
	nr_failed += retry;
	rc = nr_failed;
out:
	if (nr_succeeded)
		count_vm_events(PGMIGRATE_SUCCESS, nr_succeeded);
	if (nr_failed)
		count_vm_events(PGMIGRATE_FAIL, nr_failed);
	trace_mm_migrate_pages(nr_succeeded, nr_failed, mode, reason);

	if (!swapwrite)
		current->flags &= ~PF_SWAPWRITE;

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.migrate_pages_cleanup_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	return rc;
}

#ifdef CONFIG_NUMA

static int store_status(int __user *status, int start, int value, int nr)
{
	while (nr-- > 0) {
		if (put_user(value, status + start))
			return -EFAULT;
		start++;
	}

	return 0;
}

// IS_PMEM_NODE[x] stores is NUMA node x is PMEM memory only NUMA node
char IS_PMEM_NODE[MAX_NUMNODES];
EXPORT_SYMBOL(IS_PMEM_NODE);

void init_closest_cpu_node_for_pmem_list_kernel(void);
/**
 * @brief Initializes CLOSEST_CPU_NODE_FOR_PMEM array.
 * CLOSEST_CPU_NODE_FOR_PMEM[x] contains id of a cpu which 
 * is on the same socket as a NUMA node x if node x is 
 * PMEM NUMA node. Otherwise it stores -1.
 */
void init_closest_cpu_node_for_pmem_list_kernel(){
	if(CLOSEST_CPU_NODE_FOR_PMEM_INITIALIZED)
		return;
		
	int i, j, cpu, nid, cmin;
	char *is_cpu_node = (char*) kmalloc(MAX_NUMNODES, GFP_KERNEL);

	if(!is_cpu_node){
		printk("Unable to initialize CLOSEST_CPU_NODE_FOR_PMEM: kernel memory allocation failed!\n");
		return;
	}

	int *node_to_cpu = (int*) kmalloc(MAX_NUMNODES*sizeof(int), GFP_KERNEL);
	if(!node_to_cpu){
		printk("Unable to initialize CLOSEST_CPU_NODE_FOR_PMEM: kernel memory allocation failed!\n");
		kfree(is_cpu_node);
		return;
	}

	memset(is_cpu_node, 0, MAX_NUMNODES);
	memset(node_to_cpu, -1, MAX_NUMNODES*sizeof(int));
	
	for_each_present_cpu(cpu) {
		nid = cpu_to_node(cpu);
		if (nid>=0 && nid<MAX_NUMNODES){
			is_cpu_node[nid] = 1;
			node_to_cpu[nid] = cpu;
		}
	}

	// find for all pmem nodes the closest cpu node
	for(i=0; i<MAX_NUMNODES; i++){
		if(!IS_PMEM_NODE[i]){
			CLOSEST_CPU_NODE_FOR_PMEM[i] = -1;
			continue;
		}
		/**
		 * Initialize cmin to 256 which is more than highest
		 * possible distance between any two numa nodes.
		 */
		cmin = 256;
		for(j=0; j<MAX_NUMNODES; j++){
			if(is_cpu_node[j] && cmin>node_distance(i, j)){
				cmin = node_distance(i, j);
				CLOSEST_CPU_NODE_FOR_PMEM[i] = node_to_cpu[j];
			}
		}
	}

	CLOSEST_CPU_NODE_FOR_PMEM_INITIALIZED = 1;
	kfree(node_to_cpu);
	kfree(is_cpu_node);
}

// returns id of closest cpu to the given NUMA node
int get_nearest_cpu_node(int node){
	init_closest_cpu_node_for_pmem_list_kernel();
	if(CLOSEST_CPU_NODE_FOR_PMEM_INITIALIZED==0 || node < 0 || node >= MAX_NUMNODES)
		return -1;
	return CLOSEST_CPU_NODE_FOR_PMEM[node];
}

static int do_move_pages_to_node(struct mm_struct *mm,
		struct list_head *pagelist, int node,
		bool migrate_mt, bool migrate_dma, bool migrate_concur)
{
	int err;

	if (list_empty(pagelist))
		return 0;

	if (migrate_concur) {
		err = migrate_pages_concur(pagelist, alloc_new_node_page, NULL, node,
				MIGRATE_SYNC | (migrate_mt ? MIGRATE_MT : MIGRATE_SINGLETHREAD) |
				(migrate_dma ? MIGRATE_DMA : MIGRATE_SINGLETHREAD),
				MR_SYSCALL);

	} else {
		err = migrate_pages(pagelist, alloc_new_node_page, NULL, node,
				MIGRATE_SYNC | (migrate_mt ? MIGRATE_MT : MIGRATE_SINGLETHREAD) |
				(migrate_dma ? MIGRATE_DMA : MIGRATE_SINGLETHREAD),
				MR_SYSCALL);
	}
	if (err)
		putback_movable_pages(pagelist);
	return err;
}

/*
 * Resolves the given address to a struct page, isolates it from the LRU and
 * puts it to the given pagelist.
 * Returns:
 *     errno - if the page cannot be found/isolated
 *     0 - when it doesn't have to be migrated because it is already on the
 *         target node
 *     1 - when it has been queued
 */
static int add_page_for_migration(struct mm_struct *mm, unsigned long addr,
		int node, struct list_head *pagelist, bool migrate_all)
{
	struct vm_area_struct *vma;
	struct page *page;
	unsigned int follflags;
	int err;

	down_read(&mm->mmap_sem);
	err = -EFAULT;
	vma = find_vma(mm, addr);
	if (!vma || addr < vma->vm_start || !vma_migratable(vma))
		goto out;

	/* FOLL_DUMP to ignore special (like zero) pages */
	follflags = FOLL_GET | FOLL_DUMP;
	page = follow_page(vma, addr, follflags);

	err = PTR_ERR(page);
	if (IS_ERR(page))
		goto out;

	err = -ENOENT;
	if (!page)
		goto out;

	err = 0;
	if (page_to_nid(page) == node)
		goto out_putpage;

	err = -EACCES;
	if (page_mapcount(page) > 1 && !migrate_all)
		goto out_putpage;

	if (PageHuge(page)) {
		if (PageHead(page)) {
			isolate_huge_page(page, pagelist);
			err = 1;
		}
	} else {
		struct page *head;

		head = compound_head(page);
		err = isolate_lru_page(head);
		if (err)
			goto out_putpage;

		err = 1;
		list_add_tail(&head->lru, pagelist);
		mod_node_page_state(page_pgdat(head),
			NR_ISOLATED_ANON + page_is_file_cache(head),
			hpage_nr_pages(head));
	}
out_putpage:
	/*
	 * Either remove the duplicate refcount from
	 * isolate_lru_page() or drop the page ref if it was
	 * not isolated.
	 */
	put_page(page);
out:
	up_read(&mm->mmap_sem);
	return err;
}

/*
 * Migrate an array of page address onto an array of nodes and fill
 * the corresponding array of status.
 */
static int do_pages_move(struct mm_struct *mm, nodemask_t task_nodes,
			 unsigned long nr_pages,
			 const void __user * __user *pages,
			 const int __user *nodes,
			 int __user *status, int flags)
{
	int current_node = NUMA_NO_NODE;
	LIST_HEAD(pagelist);
	int start, i;
	int err = 0, err1;
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	u64 timestamp;
#endif

	migrate_prep();

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.migrate_prep_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	down_read(&mm->mmap_sem);

	for (i = start = 0; i < nr_pages; i++) {
		const void __user *p;
		unsigned long addr;
		int node;

		err = -EFAULT;
		if (get_user(p, pages + i))
			goto out_flush;
		if (get_user(node, nodes + i))
			goto out_flush;
		addr = (unsigned long)untagged_addr(p);

		err = -ENODEV;
		if (node < 0 || node >= MAX_NUMNODES)
			goto out_flush;
		if (!node_state(node, N_MEMORY))
			goto out_flush;

		err = -EACCES;
		if (!node_isset(node, task_nodes))
			goto out_flush;

		if (current_node == NUMA_NO_NODE) {
			current_node = node;
			start = i;
		} else if (node != current_node) {
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
			timestamp = rdtsc();
			current->move_pages_breakdown.form_page_node_info_cycles += timestamp -
				current->move_pages_breakdown.last_timestamp;
			current->move_pages_breakdown.last_timestamp = timestamp;
#endif

			err = do_move_pages_to_node(mm, &pagelist, current_node,
				flags & MPOL_MF_MOVE_MT, flags & MPOL_MF_MOVE_DMA,
				flags & MPOL_MF_MOVE_CONCUR);
			if (err) {
				/*
				 * Positive err means the number of failed
				 * pages to migrate.  Since we are going to
				 * abort and return the number of non-migrated
				 * pages, so need to incude the rest of the
				 * nr_pages that have not been attempted as
				 * well.
				 */
				if (err > 0)
					err += nr_pages - i - 1;
				goto out;
			}
			err = store_status(status, start, current_node, i - start);
			if (err)
				goto out;
			start = i;
			current_node = node;

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
			timestamp = rdtsc();
			current->move_pages_breakdown.store_page_status_cycles += timestamp -
				current->move_pages_breakdown.last_timestamp;
			current->move_pages_breakdown.last_timestamp = timestamp;
#endif
		}

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.form_page_node_info_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif
		/*
		 * Errors in the page lookup or isolation are not fatal and we simply
		 * report them via status
		 */
		err = add_page_for_migration(mm, addr, current_node,
				&pagelist, flags & MPOL_MF_MOVE_ALL);

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.form_physical_page_list_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif
		if (!err) {
			/* The page is already on the target node */
			err = store_status(status, i, current_node, 1);
			if (err)
				goto out_flush;
			continue;
		} else if (err > 0) {
			/* The page is successfully queued for migration */
			continue;
		}

		err = store_status(status, i, err, 1);
		if (err)
			goto out_flush;

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.form_page_node_info_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif

		err = do_move_pages_to_node(mm, &pagelist, current_node,
				flags & MPOL_MF_MOVE_MT, flags & MPOL_MF_MOVE_DMA,
				flags & MPOL_MF_MOVE_CONCUR);
		if (err) {
			if (err > 0)
				err += nr_pages - i - 1;
			goto out;
		}
		if (i > start) {
			err = store_status(status, start, current_node, i - start);
			if (err)
				goto out;
		}
		current_node = NUMA_NO_NODE;

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
		timestamp = rdtsc();
		current->move_pages_breakdown.store_page_status_cycles += timestamp -
			current->move_pages_breakdown.last_timestamp;
		current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	}
out_flush:
	if (list_empty(&pagelist))
	{
		up_read(&mm->mmap_sem);
		return err;
	}

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.form_page_node_info_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	/* Make sure we do not overwrite the existing error */
	err1 = do_move_pages_to_node(mm, &pagelist, current_node,
				flags & MPOL_MF_MOVE_MT, flags & MPOL_MF_MOVE_DMA,
				flags & MPOL_MF_MOVE_CONCUR);
	/*
	 * Don't have to report non-attempted pages here since:
	 *     - If the above loop is done gracefully all pages have been
	 *       attempted.
	 *     - If the above loop is aborted it means a fatal error
	 *       happened, should return ret.
	 */
	if (!err1)
		err1 = store_status(status, start, current_node, i - start);
	if (err >= 0)
		err = err1;

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.store_page_status_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

out:
	up_read(&mm->mmap_sem);
	return err;
}

/*
 * Determine the nodes of an array of pages and store it in an array of status.
 */
static void do_pages_stat_array(struct mm_struct *mm, unsigned long nr_pages,
				const void __user **pages, int *status)
{
	unsigned long i;

	down_read(&mm->mmap_sem);

	for (i = 0; i < nr_pages; i++) {
		unsigned long addr = (unsigned long)(*pages);
		struct vm_area_struct *vma;
		struct page *page;
		int err = -EFAULT;

		vma = find_vma(mm, addr);
		if (!vma || addr < vma->vm_start)
			goto set_status;

		/* FOLL_DUMP to ignore special (like zero) pages */
		page = follow_page(vma, addr, FOLL_DUMP);

		err = PTR_ERR(page);
		if (IS_ERR(page))
			goto set_status;

		err = page ? page_to_nid(page) : -ENOENT;
set_status:
		*status = err;

		pages++;
		status++;
	}

	up_read(&mm->mmap_sem);
}

/*
 * Determine the nodes of a user array of pages and store it in
 * a user array of status.
 */
static int do_pages_stat(struct mm_struct *mm, unsigned long nr_pages,
			 const void __user * __user *pages,
			 int __user *status)
{
#define DO_PAGES_STAT_CHUNK_NR 16
	const void __user *chunk_pages[DO_PAGES_STAT_CHUNK_NR];
	int chunk_status[DO_PAGES_STAT_CHUNK_NR];

	while (nr_pages) {
		unsigned long chunk_nr;

		chunk_nr = nr_pages;
		if (chunk_nr > DO_PAGES_STAT_CHUNK_NR)
			chunk_nr = DO_PAGES_STAT_CHUNK_NR;

		if (copy_from_user(chunk_pages, pages, chunk_nr * sizeof(*chunk_pages)))
			break;

		do_pages_stat_array(mm, chunk_nr, chunk_pages, chunk_status);

		if (copy_to_user(status, chunk_status, chunk_nr * sizeof(*status)))
			break;

		pages += chunk_nr;
		status += chunk_nr;
		nr_pages -= chunk_nr;
	}
	return nr_pages ? -EFAULT : 0;
}

/*
 * Move a list of pages in the address space of the currently executing
 * process.
 */
static int kernel_move_pages(pid_t pid, unsigned long nr_pages,
			     const void __user * __user *pages,
			     const int __user *nodes,
			     int __user *status, int flags)
{
	struct task_struct *task;
	struct mm_struct *mm;
	int err;
	nodemask_t task_nodes;
#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	u64 timestamp = rdtsc();

	current->move_pages_breakdown.syscall_timestamp += timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	/* Check flags */
	if (flags & ~(MPOL_MF_MOVE|MPOL_MF_MOVE_ALL|
				  MPOL_MF_MOVE_DMA|MPOL_MF_MOVE_MT|
				  MPOL_MF_MOVE_CONCUR))
		return -EINVAL;

	if ((flags & MPOL_MF_MOVE_ALL) && !capable(CAP_SYS_NICE))
		return -EPERM;

	/* Find the mm_struct */
	rcu_read_lock();
	task = pid ? find_task_by_vpid(pid) : current;
	if (!task) {
		rcu_read_unlock();
		return -ESRCH;
	}
	get_task_struct(task);

	/*
	 * Check if this process has the right to modify the specified
	 * process. Use the regular "ptrace_may_access()" checks.
	 */
	if (!ptrace_may_access(task, PTRACE_MODE_READ_REALCREDS)) {
		rcu_read_unlock();
		err = -EPERM;
		goto out;
	}
	rcu_read_unlock();

 	err = security_task_movememory(task);
 	if (err)
		goto out;

	task_nodes = cpuset_mems_allowed(task);
	mm = get_task_mm(task);
	put_task_struct(task);

	if (!mm)
		return -EINVAL;

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.check_rights_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	if (nodes)
		err = do_pages_move(mm, task_nodes, nr_pages, pages,
				    nodes, status, flags);
	else
		err = do_pages_stat(mm, nr_pages, pages, status);

	mmput(mm);

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.return_to_syscall_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	return err;

out:
	put_task_struct(task);

#ifdef CONFIG_PAGE_MIGRATION_PROFILE
	timestamp = rdtsc();
	current->move_pages_breakdown.return_to_syscall_cycles += timestamp -
		current->move_pages_breakdown.last_timestamp;
	current->move_pages_breakdown.last_timestamp = timestamp;
#endif

	return err;
}

SYSCALL_DEFINE6(move_pages, pid_t, pid, unsigned long, nr_pages,
		const void __user * __user *, pages,
		const int __user *, nodes,
		int __user *, status, int, flags)
{
	return kernel_move_pages(pid, nr_pages, pages, nodes, status, flags);
}

#ifdef CONFIG_COMPAT
COMPAT_SYSCALL_DEFINE6(move_pages, pid_t, pid, compat_ulong_t, nr_pages,
		       compat_uptr_t __user *, pages32,
		       const int __user *, nodes,
		       int __user *, status,
		       int, flags)
{
	const void __user * __user *pages;
	int i;

	pages = compat_alloc_user_space(nr_pages * sizeof(void *));
	for (i = 0; i < nr_pages; i++) {
		compat_uptr_t p;

		if (get_user(p, pages32 + i) ||
			put_user(compat_ptr(p), pages + i))
			return -EFAULT;
	}
	return kernel_move_pages(pid, nr_pages, pages, nodes, status, flags);
}
#endif /* CONFIG_COMPAT */

#ifdef CONFIG_NUMA_BALANCING
/*
 * Returns true if this is a safe migration target node for misplaced NUMA
 * pages. Currently it only checks the watermarks which crude
 */
static bool migrate_balanced_pgdat(struct pglist_data *pgdat,
				   unsigned long nr_migrate_pages)
{
	int z;

	for (z = pgdat->nr_zones - 1; z >= 0; z--) {
		struct zone *zone = pgdat->node_zones + z;

		if (!populated_zone(zone))
			continue;

		/* Avoid waking kswapd by allocating pages_to_migrate pages. */
		if (!zone_watermark_ok(zone, 0,
				       high_wmark_pages(zone) +
				       nr_migrate_pages,
				       ZONE_MOVABLE, 0))
			continue;
		return true;
	}
	return false;
}

static struct page *alloc_misplaced_dst_page(struct page *page,
					   unsigned long data)
{
	int nid = (int) data;
	struct page *newpage;

	newpage = __alloc_pages_node(nid,
					 (GFP_HIGHUSER_MOVABLE |
					  __GFP_THISNODE | __GFP_NOMEMALLOC |
					  __GFP_NORETRY | __GFP_NOWARN) &
					 ~__GFP_RECLAIM, 0);

	return newpage;
}

static int numamigrate_isolate_page(pg_data_t *pgdat, struct page *page)
{
	int page_lru;

	VM_BUG_ON_PAGE(compound_order(page) && !PageTransHuge(page), page);

	/* Avoid migrating to a node that is nearly full */
	if (!migrate_balanced_pgdat(pgdat, compound_nr(page)))
		return 0;

	if (isolate_lru_page(page))
		return 0;

	/*
	 * migrate_misplaced_transhuge_page() skips page migration's usual
	 * check on page_count(), so we must do it here, now that the page
	 * has been isolated: a GUP pin, or any other pin, prevents migration.
	 * The expected page count is 3: 1 for page's mapcount and 1 for the
	 * caller's pin and 1 for the reference taken by isolate_lru_page().
	 */
	if (PageTransHuge(page) && page_count(page) != 3) {
		putback_lru_page(page);
		return 0;
	}

	page_lru = page_is_file_cache(page);
	mod_node_page_state(page_pgdat(page), NR_ISOLATED_ANON + page_lru,
				hpage_nr_pages(page));

	/*
	 * Isolating the page has taken another reference, so the
	 * caller's reference can be safely dropped without the page
	 * disappearing underneath us during migration.
	 */
	put_page(page);
	return 1;
}

bool pmd_trans_migrating(pmd_t pmd)
{
	struct page *page = pmd_page(pmd);
	return PageLocked(page);
}

/*
 * Attempt to migrate a misplaced page to the specified destination
 * node. Caller is expected to have an elevated reference count on
 * the page that will be dropped by this function before returning.
 */
int migrate_misplaced_page(struct page *page, struct vm_area_struct *vma,
			   int node)
{
	pg_data_t *pgdat = NODE_DATA(node);
	int isolated;
	int nr_remaining;
	LIST_HEAD(migratepages);

	/*
	 * Don't migrate file pages that are mapped in multiple processes
	 * with execute permissions as they are probably shared libraries.
	 */
	if (page_mapcount(page) != 1 && page_is_file_cache(page) &&
	    (vma->vm_flags & VM_EXEC))
		goto out;

	/*
	 * Also do not migrate dirty pages as not all filesystems can move
	 * dirty pages in MIGRATE_ASYNC mode which is a waste of cycles.
	 */
	if (page_is_file_cache(page) && PageDirty(page))
		goto out;

	isolated = numamigrate_isolate_page(pgdat, page);
	if (!isolated)
		goto out;

	list_add(&page->lru, &migratepages);
	nr_remaining = migrate_pages(&migratepages, alloc_misplaced_dst_page,
				     NULL, node, MIGRATE_ASYNC,
				     MR_NUMA_MISPLACED);
	if (nr_remaining) {
		if (!list_empty(&migratepages)) {
			list_del(&page->lru);
			dec_node_page_state(page, NR_ISOLATED_ANON +
					page_is_file_cache(page));
			putback_lru_page(page);
		}
		isolated = 0;
	} else
		count_vm_numa_event(NUMA_PAGE_MIGRATE);
	BUG_ON(!list_empty(&migratepages));
	return isolated;

out:
	put_page(page);
	return 0;
}
#endif /* CONFIG_NUMA_BALANCING */

#if defined(CONFIG_NUMA_BALANCING) && defined(CONFIG_TRANSPARENT_HUGEPAGE)
/*
 * Migrates a THP to a given target node. page must be locked and is unlocked
 * before returning.
 */
int migrate_misplaced_transhuge_page(struct mm_struct *mm,
				struct vm_area_struct *vma,
				pmd_t *pmd, pmd_t entry,
				unsigned long address,
				struct page *page, int node)
{
	spinlock_t *ptl;
	pg_data_t *pgdat = NODE_DATA(node);
	int isolated = 0;
	struct page *new_page = NULL;
	int page_lru = page_is_file_cache(page);
	unsigned long start = address & HPAGE_PMD_MASK;

	new_page = alloc_pages_node(node,
		(GFP_TRANSHUGE_LIGHT | __GFP_THISNODE),
		HPAGE_PMD_ORDER);
	if (!new_page)
		goto out_fail;
	prep_transhuge_page(new_page);

	isolated = numamigrate_isolate_page(pgdat, page);
	if (!isolated) {
		put_page(new_page);
		goto out_fail;
	}

	/* Prepare a page as a migration target */
	__SetPageLocked(new_page);
	if (PageSwapBacked(page))
		__SetPageSwapBacked(new_page);

	/* anon mapping, we can simply copy page->mapping to the new page: */
	new_page->mapping = page->mapping;
	new_page->index = page->index;
	/* flush the cache before copying using the kernel virtual address */
	flush_cache_range(vma, start, start + HPAGE_PMD_SIZE);
	migrate_page_copy(new_page, page, MIGRATE_SINGLETHREAD);
	WARN_ON(PageLRU(new_page));

	/* Recheck the target PMD */
	ptl = pmd_lock(mm, pmd);
	if (unlikely(!pmd_same(*pmd, entry) || !page_ref_freeze(page, 2))) {
		spin_unlock(ptl);

		/* Reverse changes made by migrate_page_copy() */
		if (TestClearPageActive(new_page))
			SetPageActive(page);
		if (TestClearPageUnevictable(new_page))
			SetPageUnevictable(page);

		unlock_page(new_page);
		put_page(new_page);		/* Free it */

		/* Retake the callers reference and putback on LRU */
		get_page(page);
		putback_lru_page(page);
		mod_node_page_state(page_pgdat(page),
			 NR_ISOLATED_ANON + page_lru, -HPAGE_PMD_NR);

		goto out_unlock;
	}

	entry = mk_huge_pmd(new_page, vma->vm_page_prot);
	entry = maybe_pmd_mkwrite(pmd_mkdirty(entry), vma);

	/*
	 * Overwrite the old entry under pagetable lock and establish
	 * the new PTE. Any parallel GUP will either observe the old
	 * page blocking on the page lock, block on the page table
	 * lock or observe the new page. The SetPageUptodate on the
	 * new page and page_add_new_anon_rmap guarantee the copy is
	 * visible before the pagetable update.
	 */
	page_add_anon_rmap(new_page, vma, start, true);
	/*
	 * At this point the pmd is numa/protnone (i.e. non present) and the TLB
	 * has already been flushed globally.  So no TLB can be currently
	 * caching this non present pmd mapping.  There's no need to clear the
	 * pmd before doing set_pmd_at(), nor to flush the TLB after
	 * set_pmd_at().  Clearing the pmd here would introduce a race
	 * condition against MADV_DONTNEED, because MADV_DONTNEED only holds the
	 * mmap_sem for reading.  If the pmd is set to NULL at any given time,
	 * MADV_DONTNEED won't wait on the pmd lock and it'll skip clearing this
	 * pmd.
	 */
	set_pmd_at(mm, start, pmd, entry);
	update_mmu_cache_pmd(vma, address, &entry);

	page_ref_unfreeze(page, 2);
	mlock_migrate_page(new_page, page);
	page_remove_rmap(page, true);
	set_page_owner_migrate_reason(new_page, MR_NUMA_MISPLACED);

	spin_unlock(ptl);

	/* Take an "isolate" reference and put new page on the LRU. */
	get_page(new_page);
	putback_lru_page(new_page);

	unlock_page(new_page);
	unlock_page(page);
	put_page(page);			/* Drop the rmap reference */
	put_page(page);			/* Drop the LRU isolation reference */

	count_vm_events(PGMIGRATE_SUCCESS, HPAGE_PMD_NR);
	count_vm_numa_events(NUMA_PAGE_MIGRATE, HPAGE_PMD_NR);

	mod_node_page_state(page_pgdat(page),
			NR_ISOLATED_ANON + page_lru,
			-HPAGE_PMD_NR);
	return isolated;

out_fail:
	count_vm_events(PGMIGRATE_FAIL, HPAGE_PMD_NR);
	ptl = pmd_lock(mm, pmd);
	if (pmd_same(*pmd, entry)) {
		entry = pmd_modify(entry, vma->vm_page_prot);
		set_pmd_at(mm, start, pmd, entry);
		update_mmu_cache_pmd(vma, address, &entry);
	}
	spin_unlock(ptl);

out_unlock:
	unlock_page(page);
	put_page(page);
	return 0;
}
#endif /* CONFIG_NUMA_BALANCING */

#endif /* CONFIG_NUMA */

#ifdef CONFIG_DEVICE_PRIVATE
static int migrate_vma_collect_hole(unsigned long start,
				    unsigned long end,
				    __always_unused int depth,
				    struct mm_walk *walk)
{
	struct migrate_vma *migrate = walk->private;
	unsigned long addr;

	for (addr = start; addr < end; addr += PAGE_SIZE) {
		migrate->src[migrate->npages] = MIGRATE_PFN_MIGRATE;
		migrate->dst[migrate->npages] = 0;
		migrate->npages++;
		migrate->cpages++;
	}

	return 0;
}

static int migrate_vma_collect_skip(unsigned long start,
				    unsigned long end,
				    struct mm_walk *walk)
{
	struct migrate_vma *migrate = walk->private;
	unsigned long addr;

	for (addr = start; addr < end; addr += PAGE_SIZE) {
		migrate->dst[migrate->npages] = 0;
		migrate->src[migrate->npages++] = 0;
	}

	return 0;
}

static int migrate_vma_collect_pmd(pmd_t *pmdp,
				   unsigned long start,
				   unsigned long end,
				   struct mm_walk *walk)
{
	struct migrate_vma *migrate = walk->private;
	struct vm_area_struct *vma = walk->vma;
	struct mm_struct *mm = vma->vm_mm;
	unsigned long addr = start, unmapped = 0;
	spinlock_t *ptl;
	pte_t *ptep;

again:
	if (pmd_none(*pmdp))
		return migrate_vma_collect_hole(start, end, -1, walk);

	if (pmd_trans_huge(*pmdp)) {
		struct page *page;

		ptl = pmd_lock(mm, pmdp);
		if (unlikely(!pmd_trans_huge(*pmdp))) {
			spin_unlock(ptl);
			goto again;
		}

		page = pmd_page(*pmdp);
		if (is_huge_zero_page(page)) {
			spin_unlock(ptl);
			split_huge_pmd(vma, pmdp, addr);
			if (pmd_trans_unstable(pmdp))
				return migrate_vma_collect_skip(start, end,
								walk);
		} else {
			int ret;

			get_page(page);
			spin_unlock(ptl);
			if (unlikely(!trylock_page(page)))
				return migrate_vma_collect_skip(start, end,
								walk);
			ret = split_huge_page(page);
			unlock_page(page);
			put_page(page);
			if (ret)
				return migrate_vma_collect_skip(start, end,
								walk);
			if (pmd_none(*pmdp))
				return migrate_vma_collect_hole(start, end, -1,
								walk);
		}
	}

	if (unlikely(pmd_bad(*pmdp)))
		return migrate_vma_collect_skip(start, end, walk);

	ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);
	arch_enter_lazy_mmu_mode();

	for (; addr < end; addr += PAGE_SIZE, ptep++) {
		unsigned long mpfn, pfn;
		struct page *page;
		swp_entry_t entry;
		pte_t pte;

		pte = *ptep;

		if (pte_none(pte)) {
			mpfn = MIGRATE_PFN_MIGRATE;
			migrate->cpages++;
			goto next;
		}

		if (!pte_present(pte)) {
			mpfn = 0;

			/*
			 * Only care about unaddressable device page special
			 * page table entry. Other special swap entries are not
			 * migratable, and we ignore regular swapped page.
			 */
			entry = pte_to_swp_entry(pte);
			if (!is_device_private_entry(entry))
				goto next;

			page = device_private_entry_to_page(entry);
			mpfn = migrate_pfn(page_to_pfn(page)) |
					MIGRATE_PFN_MIGRATE;
			if (is_write_device_private_entry(entry))
				mpfn |= MIGRATE_PFN_WRITE;
		} else {
			pfn = pte_pfn(pte);
			if (is_zero_pfn(pfn)) {
				mpfn = MIGRATE_PFN_MIGRATE;
				migrate->cpages++;
				goto next;
			}
			page = vm_normal_page(migrate->vma, addr, pte);
			mpfn = migrate_pfn(pfn) | MIGRATE_PFN_MIGRATE;
			mpfn |= pte_write(pte) ? MIGRATE_PFN_WRITE : 0;
		}

		/* FIXME support THP */
		if (!page || !page->mapping || PageTransCompound(page)) {
			mpfn = 0;
			goto next;
		}

		/*
		 * By getting a reference on the page we pin it and that blocks
		 * any kind of migration. Side effect is that it "freezes" the
		 * pte.
		 *
		 * We drop this reference after isolating the page from the lru
		 * for non device page (device page are not on the lru and thus
		 * can't be dropped from it).
		 */
		get_page(page);
		migrate->cpages++;

		/*
		 * Optimize for the common case where page is only mapped once
		 * in one process. If we can lock the page, then we can safely
		 * set up a special migration page table entry now.
		 */
		if (trylock_page(page)) {
			pte_t swp_pte;

			mpfn |= MIGRATE_PFN_LOCKED;
			ptep_get_and_clear(mm, addr, ptep);

			/* Setup special migration page table entry */
			entry = make_migration_entry(page, mpfn &
						     MIGRATE_PFN_WRITE);
			swp_pte = swp_entry_to_pte(entry);
			if (pte_soft_dirty(pte))
				swp_pte = pte_swp_mksoft_dirty(swp_pte);
			set_pte_at(mm, addr, ptep, swp_pte);

			/*
			 * This is like regular unmap: we remove the rmap and
			 * drop page refcount. Page won't be freed, as we took
			 * a reference just above.
			 */
			page_remove_rmap(page, false);
			put_page(page);

			if (pte_present(pte))
				unmapped++;
		}

next:
		migrate->dst[migrate->npages] = 0;
		migrate->src[migrate->npages++] = mpfn;
	}
	arch_leave_lazy_mmu_mode();
	pte_unmap_unlock(ptep - 1, ptl);

	/* Only flush the TLB if we actually modified any entries */
	if (unmapped)
		flush_tlb_range(walk->vma, start, end);

	return 0;
}

static const struct mm_walk_ops migrate_vma_walk_ops = {
	.pmd_entry		= migrate_vma_collect_pmd,
	.pte_hole		= migrate_vma_collect_hole,
};

/*
 * migrate_vma_collect() - collect pages over a range of virtual addresses
 * @migrate: migrate struct containing all migration information
 *
 * This will walk the CPU page table. For each virtual address backed by a
 * valid page, it updates the src array and takes a reference on the page, in
 * order to pin the page until we lock it and unmap it.
 */
static void migrate_vma_collect(struct migrate_vma *migrate)
{
	struct mmu_notifier_range range;

	mmu_notifier_range_init(&range, MMU_NOTIFY_CLEAR, 0, NULL,
			migrate->vma->vm_mm, migrate->start, migrate->end);
	mmu_notifier_invalidate_range_start(&range);

	walk_page_range(migrate->vma->vm_mm, migrate->start, migrate->end,
			&migrate_vma_walk_ops, migrate);

	mmu_notifier_invalidate_range_end(&range);
	migrate->end = migrate->start + (migrate->npages << PAGE_SHIFT);
}

/*
 * migrate_vma_check_page() - check if page is pinned or not
 * @page: struct page to check
 *
 * Pinned pages cannot be migrated. This is the same test as in
 * migrate_page_move_mapping(), except that here we allow migration of a
 * ZONE_DEVICE page.
 */
static bool migrate_vma_check_page(struct page *page)
{
	/*
	 * One extra ref because caller holds an extra reference, either from
	 * isolate_lru_page() for a regular page, or migrate_vma_collect() for
	 * a device page.
	 */
	int extra = 1;

	/*
	 * FIXME support THP (transparent huge page), it is bit more complex to
	 * check them than regular pages, because they can be mapped with a pmd
	 * or with a pte (split pte mapping).
	 */
	if (PageCompound(page))
		return false;

	/* Page from ZONE_DEVICE have one extra reference */
	if (is_zone_device_page(page)) {
		/*
		 * Private page can never be pin as they have no valid pte and
		 * GUP will fail for those. Yet if there is a pending migration
		 * a thread might try to wait on the pte migration entry and
		 * will bump the page reference count. Sadly there is no way to
		 * differentiate a regular pin from migration wait. Hence to
		 * avoid 2 racing thread trying to migrate back to CPU to enter
		 * infinite loop (one stoping migration because the other is
		 * waiting on pte migration entry). We always return true here.
		 *
		 * FIXME proper solution is to rework migration_entry_wait() so
		 * it does not need to take a reference on page.
		 */
		return is_device_private_page(page);
	}

	/* For file back page */
	if (page_mapping(page))
		extra += 1 + page_has_private(page);

	if ((page_count(page) - extra) > page_mapcount(page))
		return false;

	return true;
}

/*
 * migrate_vma_prepare() - lock pages and isolate them from the lru
 * @migrate: migrate struct containing all migration information
 *
 * This locks pages that have been collected by migrate_vma_collect(). Once each
 * page is locked it is isolated from the lru (for non-device pages). Finally,
 * the ref taken by migrate_vma_collect() is dropped, as locked pages cannot be
 * migrated by concurrent kernel threads.
 */
static void migrate_vma_prepare(struct migrate_vma *migrate)
{
	const unsigned long npages = migrate->npages;
	const unsigned long start = migrate->start;
	unsigned long addr, i, restore = 0;
	bool allow_drain = true;

	lru_add_drain();

	for (i = 0; (i < npages) && migrate->cpages; i++) {
		struct page *page = migrate_pfn_to_page(migrate->src[i]);
		bool remap = true;

		if (!page)
			continue;

		if (!(migrate->src[i] & MIGRATE_PFN_LOCKED)) {
			/*
			 * Because we are migrating several pages there can be
			 * a deadlock between 2 concurrent migration where each
			 * are waiting on each other page lock.
			 *
			 * Make migrate_vma() a best effort thing and backoff
			 * for any page we can not lock right away.
			 */
			if (!trylock_page(page)) {
				migrate->src[i] = 0;
				migrate->cpages--;
				put_page(page);
				continue;
			}
			remap = false;
			migrate->src[i] |= MIGRATE_PFN_LOCKED;
		}

		/* ZONE_DEVICE pages are not on LRU */
		if (!is_zone_device_page(page)) {
			if (!PageLRU(page) && allow_drain) {
				/* Drain CPU's pagevec */
				lru_add_drain_all();
				allow_drain = false;
			}

			if (isolate_lru_page(page)) {
				if (remap) {
					migrate->src[i] &= ~MIGRATE_PFN_MIGRATE;
					migrate->cpages--;
					restore++;
				} else {
					migrate->src[i] = 0;
					unlock_page(page);
					migrate->cpages--;
					put_page(page);
				}
				continue;
			}

			/* Drop the reference we took in collect */
			put_page(page);
		}

		if (!migrate_vma_check_page(page)) {
			if (remap) {
				migrate->src[i] &= ~MIGRATE_PFN_MIGRATE;
				migrate->cpages--;
				restore++;

				if (!is_zone_device_page(page)) {
					get_page(page);
					putback_lru_page(page);
				}
			} else {
				migrate->src[i] = 0;
				unlock_page(page);
				migrate->cpages--;

				if (!is_zone_device_page(page))
					putback_lru_page(page);
				else
					put_page(page);
			}
		}
	}

	for (i = 0, addr = start; i < npages && restore; i++, addr += PAGE_SIZE) {
		struct page *page = migrate_pfn_to_page(migrate->src[i]);

		if (!page || (migrate->src[i] & MIGRATE_PFN_MIGRATE))
			continue;

		remove_migration_pte(page, migrate->vma, addr, page);

		migrate->src[i] = 0;
		unlock_page(page);
		put_page(page);
		restore--;
	}
}

/*
 * migrate_vma_unmap() - replace page mapping with special migration pte entry
 * @migrate: migrate struct containing all migration information
 *
 * Replace page mapping (CPU page table pte) with a special migration pte entry
 * and check again if it has been pinned. Pinned pages are restored because we
 * cannot migrate them.
 *
 * This is the last step before we call the device driver callback to allocate
 * destination memory and copy contents of original page over to new page.
 */
static void migrate_vma_unmap(struct migrate_vma *migrate)
{
	int flags = TTU_MIGRATION | TTU_IGNORE_MLOCK | TTU_IGNORE_ACCESS;
	const unsigned long npages = migrate->npages;
	const unsigned long start = migrate->start;
	unsigned long addr, i, restore = 0;

	for (i = 0; i < npages; i++) {
		struct page *page = migrate_pfn_to_page(migrate->src[i]);

		if (!page || !(migrate->src[i] & MIGRATE_PFN_MIGRATE))
			continue;

		if (page_mapped(page)) {
			try_to_unmap(page, flags);
			if (page_mapped(page))
				goto restore;
		}

		if (migrate_vma_check_page(page))
			continue;

restore:
		migrate->src[i] &= ~MIGRATE_PFN_MIGRATE;
		migrate->cpages--;
		restore++;
	}

	for (addr = start, i = 0; i < npages && restore; addr += PAGE_SIZE, i++) {
		struct page *page = migrate_pfn_to_page(migrate->src[i]);

		if (!page || (migrate->src[i] & MIGRATE_PFN_MIGRATE))
			continue;

		remove_migration_ptes(page, page, false);

		migrate->src[i] = 0;
		unlock_page(page);
		restore--;

		if (is_zone_device_page(page))
			put_page(page);
		else
			putback_lru_page(page);
	}
}

/**
 * migrate_vma_setup() - prepare to migrate a range of memory
 * @args: contains the vma, start, and and pfns arrays for the migration
 *
 * Returns: negative errno on failures, 0 when 0 or more pages were migrated
 * without an error.
 *
 * Prepare to migrate a range of memory virtual address range by collecting all
 * the pages backing each virtual address in the range, saving them inside the
 * src array.  Then lock those pages and unmap them. Once the pages are locked
 * and unmapped, check whether each page is pinned or not.  Pages that aren't
 * pinned have the MIGRATE_PFN_MIGRATE flag set (by this function) in the
 * corresponding src array entry.  Then restores any pages that are pinned, by
 * remapping and unlocking those pages.
 *
 * The caller should then allocate destination memory and copy source memory to
 * it for all those entries (ie with MIGRATE_PFN_VALID and MIGRATE_PFN_MIGRATE
 * flag set).  Once these are allocated and copied, the caller must update each
 * corresponding entry in the dst array with the pfn value of the destination
 * page and with the MIGRATE_PFN_VALID and MIGRATE_PFN_LOCKED flags set
 * (destination pages must have their struct pages locked, via lock_page()).
 *
 * Note that the caller does not have to migrate all the pages that are marked
 * with MIGRATE_PFN_MIGRATE flag in src array unless this is a migration from
 * device memory to system memory.  If the caller cannot migrate a device page
 * back to system memory, then it must return VM_FAULT_SIGBUS, which has severe
 * consequences for the userspace process, so it must be avoided if at all
 * possible.
 *
 * For empty entries inside CPU page table (pte_none() or pmd_none() is true) we
 * do set MIGRATE_PFN_MIGRATE flag inside the corresponding source array thus
 * allowing the caller to allocate device memory for those unback virtual
 * address.  For this the caller simply has to allocate device memory and
 * properly set the destination entry like for regular migration.  Note that
 * this can still fails and thus inside the device driver must check if the
 * migration was successful for those entries after calling migrate_vma_pages()
 * just like for regular migration.
 *
 * After that, the callers must call migrate_vma_pages() to go over each entry
 * in the src array that has the MIGRATE_PFN_VALID and MIGRATE_PFN_MIGRATE flag
 * set. If the corresponding entry in dst array has MIGRATE_PFN_VALID flag set,
 * then migrate_vma_pages() to migrate struct page information from the source
 * struct page to the destination struct page.  If it fails to migrate the
 * struct page information, then it clears the MIGRATE_PFN_MIGRATE flag in the
 * src array.
 *
 * At this point all successfully migrated pages have an entry in the src
 * array with MIGRATE_PFN_VALID and MIGRATE_PFN_MIGRATE flag set and the dst
 * array entry with MIGRATE_PFN_VALID flag set.
 *
 * Once migrate_vma_pages() returns the caller may inspect which pages were
 * successfully migrated, and which were not.  Successfully migrated pages will
 * have the MIGRATE_PFN_MIGRATE flag set for their src array entry.
 *
 * It is safe to update device page table after migrate_vma_pages() because
 * both destination and source page are still locked, and the mmap_sem is held
 * in read mode (hence no one can unmap the range being migrated).
 *
 * Once the caller is done cleaning up things and updating its page table (if it
 * chose to do so, this is not an obligation) it finally calls
 * migrate_vma_finalize() to update the CPU page table to point to new pages
 * for successfully migrated pages or otherwise restore the CPU page table to
 * point to the original source pages.
 */
int migrate_vma_setup(struct migrate_vma *args)
{
	long nr_pages = (args->end - args->start) >> PAGE_SHIFT;

	args->start &= PAGE_MASK;
	args->end &= PAGE_MASK;
	if (!args->vma || is_vm_hugetlb_page(args->vma) ||
	    (args->vma->vm_flags & VM_SPECIAL) || vma_is_dax(args->vma))
		return -EINVAL;
	if (nr_pages <= 0)
		return -EINVAL;
	if (args->start < args->vma->vm_start ||
	    args->start >= args->vma->vm_end)
		return -EINVAL;
	if (args->end <= args->vma->vm_start || args->end > args->vma->vm_end)
		return -EINVAL;
	if (!args->src || !args->dst)
		return -EINVAL;

	memset(args->src, 0, sizeof(*args->src) * nr_pages);
	args->cpages = 0;
	args->npages = 0;

	migrate_vma_collect(args);

	if (args->cpages)
		migrate_vma_prepare(args);
	if (args->cpages)
		migrate_vma_unmap(args);

	/*
	 * At this point pages are locked and unmapped, and thus they have
	 * stable content and can safely be copied to destination memory that
	 * is allocated by the drivers.
	 */
	return 0;

}
EXPORT_SYMBOL(migrate_vma_setup);

/*
 * This code closely matches the code in:
 *   __handle_mm_fault()
 *     handle_pte_fault()
 *       do_anonymous_page()
 * to map in an anonymous zero page but the struct page will be a ZONE_DEVICE
 * private page.
 */
static void migrate_vma_insert_page(struct migrate_vma *migrate,
				    unsigned long addr,
				    struct page *page,
				    unsigned long *src,
				    unsigned long *dst)
{
	struct vm_area_struct *vma = migrate->vma;
	struct mm_struct *mm = vma->vm_mm;
	struct mem_cgroup *memcg;
	bool flush = false;
	spinlock_t *ptl;
	pte_t entry;
	pgd_t *pgdp;
	p4d_t *p4dp;
	pud_t *pudp;
	pmd_t *pmdp;
	pte_t *ptep;

	/* Only allow populating anonymous memory */
	if (!vma_is_anonymous(vma))
		goto abort;

	pgdp = pgd_offset(mm, addr);
	p4dp = p4d_alloc(mm, pgdp, addr);
	if (!p4dp)
		goto abort;
	pudp = pud_alloc(mm, p4dp, addr);
	if (!pudp)
		goto abort;
	pmdp = pmd_alloc(mm, pudp, addr);
	if (!pmdp)
		goto abort;

	if (pmd_trans_huge(*pmdp) || pmd_devmap(*pmdp))
		goto abort;

	/*
	 * Use pte_alloc() instead of pte_alloc_map().  We can't run
	 * pte_offset_map() on pmds where a huge pmd might be created
	 * from a different thread.
	 *
	 * pte_alloc_map() is safe to use under down_write(mmap_sem) or when
	 * parallel threads are excluded by other means.
	 *
	 * Here we only have down_read(mmap_sem).
	 */
	if (pte_alloc(mm, pmdp))
		goto abort;

	/* See the comment in pte_alloc_one_map() */
	if (unlikely(pmd_trans_unstable(pmdp)))
		goto abort;

	if (unlikely(anon_vma_prepare(vma)))
		goto abort;
	if (mem_cgroup_try_charge(page, vma->vm_mm, GFP_KERNEL, &memcg, false))
		goto abort;

	/*
	 * The memory barrier inside __SetPageUptodate makes sure that
	 * preceding stores to the page contents become visible before
	 * the set_pte_at() write.
	 */
	__SetPageUptodate(page);

	if (is_zone_device_page(page)) {
		if (is_device_private_page(page)) {
			swp_entry_t swp_entry;

			swp_entry = make_device_private_entry(page, vma->vm_flags & VM_WRITE);
			entry = swp_entry_to_pte(swp_entry);
		}
	} else {
		entry = mk_pte(page, vma->vm_page_prot);
		if (vma->vm_flags & VM_WRITE)
			entry = pte_mkwrite(pte_mkdirty(entry));
	}

	ptep = pte_offset_map_lock(mm, pmdp, addr, &ptl);

	if (check_stable_address_space(mm))
		goto unlock_abort;

	if (pte_present(*ptep)) {
		unsigned long pfn = pte_pfn(*ptep);

		if (!is_zero_pfn(pfn))
			goto unlock_abort;
		flush = true;
	} else if (!pte_none(*ptep))
		goto unlock_abort;

	/*
	 * Check for userfaultfd but do not deliver the fault. Instead,
	 * just back off.
	 */
	if (userfaultfd_missing(vma))
		goto unlock_abort;

	inc_mm_counter(mm, MM_ANONPAGES);
	page_add_new_anon_rmap(page, vma, addr, false);
	mem_cgroup_commit_charge(page, memcg, false, false);
	if (!is_zone_device_page(page))
		lru_cache_add_active_or_unevictable(page, vma);
	get_page(page);

	if (flush) {
		flush_cache_page(vma, addr, pte_pfn(*ptep));
		ptep_clear_flush_notify(vma, addr, ptep);
		set_pte_at_notify(mm, addr, ptep, entry);
		update_mmu_cache(vma, addr, ptep);
	} else {
		/* No need to invalidate - it was non-present before */
		set_pte_at(mm, addr, ptep, entry);
		update_mmu_cache(vma, addr, ptep);
	}

	pte_unmap_unlock(ptep, ptl);
	*src = MIGRATE_PFN_MIGRATE;
	return;

unlock_abort:
	pte_unmap_unlock(ptep, ptl);
	mem_cgroup_cancel_charge(page, memcg, false);
abort:
	*src &= ~MIGRATE_PFN_MIGRATE;
}

/**
 * migrate_vma_pages() - migrate meta-data from src page to dst page
 * @migrate: migrate struct containing all migration information
 *
 * This migrates struct page meta-data from source struct page to destination
 * struct page. This effectively finishes the migration from source page to the
 * destination page.
 */
void migrate_vma_pages(struct migrate_vma *migrate)
{
	const unsigned long npages = migrate->npages;
	const unsigned long start = migrate->start;
	struct mmu_notifier_range range;
	unsigned long addr, i;
	bool notified = false;

	for (i = 0, addr = start; i < npages; addr += PAGE_SIZE, i++) {
		struct page *newpage = migrate_pfn_to_page(migrate->dst[i]);
		struct page *page = migrate_pfn_to_page(migrate->src[i]);
		struct address_space *mapping;
		int r;

		if (!newpage) {
			migrate->src[i] &= ~MIGRATE_PFN_MIGRATE;
			continue;
		}

		if (!page) {
			if (!(migrate->src[i] & MIGRATE_PFN_MIGRATE))
				continue;
			if (!notified) {
				notified = true;

				mmu_notifier_range_init(&range,
							MMU_NOTIFY_CLEAR, 0,
							NULL,
							migrate->vma->vm_mm,
							addr, migrate->end);
				mmu_notifier_invalidate_range_start(&range);
			}
			migrate_vma_insert_page(migrate, addr, newpage,
						&migrate->src[i],
						&migrate->dst[i]);
			continue;
		}

		mapping = page_mapping(page);

		if (is_zone_device_page(newpage)) {
			if (is_device_private_page(newpage)) {
				/*
				 * For now only support private anonymous when
				 * migrating to un-addressable device memory.
				 */
				if (mapping) {
					migrate->src[i] &= ~MIGRATE_PFN_MIGRATE;
					continue;
				}
			} else {
				/*
				 * Other types of ZONE_DEVICE page are not
				 * supported.
				 */
				migrate->src[i] &= ~MIGRATE_PFN_MIGRATE;
				continue;
			}
		}

		r = migrate_page(mapping, newpage, page, MIGRATE_SYNC | MIGRATE_SYNC_NO_COPY);
		if (r != MIGRATEPAGE_SUCCESS)
			migrate->src[i] &= ~MIGRATE_PFN_MIGRATE;
	}

	/*
	 * No need to double call mmu_notifier->invalidate_range() callback as
	 * the above ptep_clear_flush_notify() inside migrate_vma_insert_page()
	 * did already call it.
	 */
	if (notified)
		mmu_notifier_invalidate_range_only_end(&range);
}
EXPORT_SYMBOL(migrate_vma_pages);

/**
 * migrate_vma_finalize() - restore CPU page table entry
 * @migrate: migrate struct containing all migration information
 *
 * This replaces the special migration pte entry with either a mapping to the
 * new page if migration was successful for that page, or to the original page
 * otherwise.
 *
 * This also unlocks the pages and puts them back on the lru, or drops the extra
 * refcount, for device pages.
 */
void migrate_vma_finalize(struct migrate_vma *migrate)
{
	const unsigned long npages = migrate->npages;
	unsigned long i;

	for (i = 0; i < npages; i++) {
		struct page *newpage = migrate_pfn_to_page(migrate->dst[i]);
		struct page *page = migrate_pfn_to_page(migrate->src[i]);

		if (!page) {
			if (newpage) {
				unlock_page(newpage);
				put_page(newpage);
			}
			continue;
		}

		if (!(migrate->src[i] & MIGRATE_PFN_MIGRATE) || !newpage) {
			if (newpage) {
				unlock_page(newpage);
				put_page(newpage);
			}
			newpage = page;
		}

		remove_migration_ptes(page, newpage, false);
		unlock_page(page);
		migrate->cpages--;

		if (is_zone_device_page(page))
			put_page(page);
		else
			putback_lru_page(page);

		if (newpage != page) {
			unlock_page(newpage);
			if (is_zone_device_page(newpage))
				put_page(newpage);
			else
				putback_lru_page(newpage);
		}
	}
}
EXPORT_SYMBOL(migrate_vma_finalize);
#endif /* CONFIG_DEVICE_PRIVATE */
