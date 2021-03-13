#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/time.h>
#include <asm/cacheflush.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Hildenbrand");

static struct task_struct *kstream_thread;
static unsigned long kstream_max_pfn;

static bool kstream_block_valid(unsigned long start_pfn)
{
	unsigned long pfn, end_pfn = start_pfn + MAX_ORDER_NR_PAGES;
	struct zone *zone;
	struct page *page;

	BUG_ON(!IS_ALIGNED(start_pfn, MAX_ORDER_NR_PAGES));

	page = pfn_to_online_page(start_pfn);
	if (!page)
		return false;
	zone = page_zone(page);

	for (pfn = start_pfn + 1; pfn < end_pfn; pfn++) {
		page = pfn_to_online_page(pfn);
		if (!page)
			return false;

		/*
		 * alloc_contig_range() requires a single zone and no reserved
		 * pages (e.g., memory holes, early allocations).
		 */
		if (page_zone(page) != zone)
			return false;

		if (!zone_spans_pfn(zone, pfn))
			return false;

		if (PageReserved(page))
			return false;
	}
	return true;
}

static unsigned long kstream_detect_max_pfn(void)
{
	unsigned long end_pfn;
	int i;

	for_each_online_node(i)
		end_pfn = max_t(unsigned long, end_pfn, node_end_pfn(i));

	return end_pfn;
}

static uint64_t kstream_run_single_cache(uint64_t * const a, uint64_t * const b,
				 	 uint64_t * const c, int array_size)
{
	const uint64_t scalar = 3;
	uint64_t best = ULONG_MAX;
	int i, k;

	/* 10 iterations */
	for (k = 0; k < 10; k++) {
		uint64_t t, t0, t1;

		t0 = ktime_get_raw_ns();
		for (i = 0; i < array_size; i++)
			/* 2 accesses */
			c[i] = a[i];
		for (i = 0; i < array_size; i++)
			/* 2 accesses */
			b[i] = scalar * c[i];
		for (i = 0; i < array_size; i++)
			/* 3 accesses */
			c[i] = a[i] + b[i];
		for (i = 0; i < array_size; i++)
			/* 3 accesses */
			a[i] = b[i] + scalar * c[i];
		t1 = ktime_get_raw_ns();
		t = t1 - t0;

		best = min_t(uint64_t, best, t);
	}

	return best;
}

static uint64_t kstream_run_single_nocache(uint64_t * const a, uint64_t * const b,
					   uint64_t * const c, int array_size)
{
	const uint64_t scalar = 3;
	uint64_t best = ULONG_MAX;
	int i, k;

	/*
	 * Flush the cache before every memory access.
	 */

	/* 10 iterations */
	for (k = 0; k < 10; k++) {
		uint64_t t = 0, t0, t1;

		clflush_cache_range(a, array_size * sizeof(uint64_t));
		clflush_cache_range(c, array_size * sizeof(uint64_t));
		t0 = ktime_get_raw_ns();
		for (i = 0; i < array_size; i++)
			/* 2 accesses */
			c[i] = a[i];
		t1 = ktime_get_raw_ns();
		t += t1 - t0;

		clflush_cache_range(b, array_size * sizeof(uint64_t));
		clflush_cache_range(c, array_size * sizeof(uint64_t));
		t0 = ktime_get_raw_ns();
		for (i = 0; i < array_size; i++)
			/* 2 accesses */
			b[i] = scalar * c[i];
		t1 = ktime_get_raw_ns();
		t += t1 - t0;

		clflush_cache_range(a, array_size * sizeof(uint64_t));
		clflush_cache_range(b, array_size * sizeof(uint64_t));
		clflush_cache_range(c, array_size * sizeof(uint64_t));
		t0 = ktime_get_raw_ns();
		for (i = 0; i < array_size; i++)
			/* 3 accesses */
			c[i] = a[i] + b[i];
		t1 = ktime_get_raw_ns();
		t += t1 - t0;

		clflush_cache_range(a, array_size * sizeof(uint64_t));
		clflush_cache_range(b, array_size * sizeof(uint64_t));
		clflush_cache_range(c, array_size * sizeof(uint64_t));
		t0 = ktime_get_raw_ns();
		for (i = 0; i < array_size; i++)
			/* 3 accesses */
			a[i] = b[i] + scalar * c[i];
		t1 = ktime_get_raw_ns();
		t += t1 - t0;

		best = min_t(uint64_t, best, t);
	}

	return best;
}

static uint64_t kstream_run_single(unsigned long pfn)
{
	const int total_bytes = MAX_ORDER_NR_PAGES << PAGE_SHIFT;
	const int array_bytes = ALIGN_DOWN(total_bytes / 3, sizeof(uint64_t));
	const int array_size = array_bytes / sizeof(uint64_t);
	uint64_t * const a = phys_to_virt(page_to_phys(pfn_to_page(pfn)));
	uint64_t * const b = a + array_size;
	uint64_t * const c = b + array_size;
	uint64_t tc, tnc, accessed_bytes;
	int i;

	/* Initialize our array */
	for (i = 0; i < array_size; i++) {
		a[i] = 2;
		b[i] = 2;
		c[i] = 0;
	}

	tc = kstream_run_single_cache(a, b, c, array_size);
	tnc = kstream_run_single_nocache(a, b, c, array_size);

	/* Validate all values are equal */
	for (i = 1; i < array_size; i++) {
		if (a[i - 1] != a[i] ||
		    b[i - 1] != b[i] ||
		    c[i - 1] != c[i]) {
			pr_err("Mismatch detected for PFN %ld", pfn);
			return -EIO;
		}
	}

	/* We perform 10 individual memory accesses to all elements in our array. */
	accessed_bytes = 10 * sizeof(uint64_t) * array_size;

	printk("[0x%px - 0x%px] %lld MB/s / %lld MB/s",
		(void *) (pfn << PAGE_SHIFT),
		(void *) (((pfn + MAX_ORDER_NR_PAGES) << PAGE_SHIFT) - 1),
		accessed_bytes * 1000 / tnc,
		accessed_bytes * 1000 / tc);
	
	return 0;
}

static int kstream_fn(void *opaque)
{
	unsigned long pfn = 0;
	int ret;

	for (pfn = 0; pfn < kstream_max_pfn; pfn += MAX_ORDER_NR_PAGES) {
		if (kthread_should_stop())
			break;
		if (!kstream_block_valid(pfn))
			continue;

		ret = alloc_contig_range(pfn, pfn + MAX_ORDER_NR_PAGES,
					 MIGRATE_MOVABLE, GFP_KERNEL);
		if (ret)
			continue;
		kstream_run_single(pfn);
		free_contig_range(pfn, MAX_ORDER_NR_PAGES);
		cond_resched();
	}


	/* Wait forever until we're told to exit. */
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
		set_current_state(TASK_RUNNING);
	}

	return 0;
}

int __init kstream_init(void)
{
	kstream_max_pfn = kstream_detect_max_pfn();

	kstream_thread = kthread_run(kstream_fn, NULL, "kstream");
	if (!kstream_thread)
		return -ENOMEM;

	return 0;
}

void __exit kstream_cleanup(void)
{
	kthread_stop(kstream_thread);
}

module_init(kstream_init);
module_exit(kstream_cleanup);
