/*
 * Benchmarking page allocator: Cross CPU moving cost
 *
 * This benchmark tried to isolate the cost associated with allocating
 * a page on one CPU and freeing it on another.
 *
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/time.h>
#include <linux/time_bench.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/list.h>
#include <linux/ptr_ring.h>

static int verbose=1;

/* Quick and dirty way to unselect some of the benchmark tests, by
 * encoding this in a module parameter flag.  This is useful when
 * wanting to perf benchmark a specific benchmark test.
 *
 * Hint: Bash shells support writing binary number like: $((2#101010))
 * Use like:
 *  modprobe page_bench05_cross_cpu loops=$((10**7))  run_flags=$((2#010))
 */
static unsigned long run_flags = 0xFFFFFFFF;
module_param(run_flags, ulong, 0);
MODULE_PARM_DESC(run_flags, "Hack way to limit bench to run");
/* Count the bit number from the enum */
enum benchmark_bit {
	bit_run_bench_order0_compare,
	bit_run_bench_ptr_ring_baseline,
	bit_run_bench_cross_cpu_page_alloc_put,
};
#define bit(b)	(1 << (b))
#define run_or_return(b) do { if (!(run_flags & (bit(b)))) return; } while (0)

#define DEFAULT_ORDER 0
static int page_order = DEFAULT_ORDER;
module_param(page_order, uint, 0);
MODULE_PARM_DESC(page_order, "Parameter page order to use in bench");

static uint32_t loops = 1000000;
module_param(loops, uint, 0);
MODULE_PARM_DESC(loops, "Iteration loops");

/* Most simple case for comparison */
static int time_single_cpu_page_alloc_put(
	struct time_bench_record *rec, void *data)
{
	gfp_t gfp_mask = (GFP_ATOMIC | ___GFP_NORETRY);
	struct page *my_page;
	int i;

	time_bench_start(rec);
	/** Loop to measure **/
	for (i = 0; i < rec->loops; i++) {
		my_page = alloc_pages(gfp_mask, page_order);
		if (unlikely(my_page == NULL))
			return 0;
		put_page(my_page);
	}
	time_bench_stop(rec, i);
	return i;
}

void noinline run_bench_order0_compare(uint32_t loops)
{
	run_or_return(bit_run_bench_order0_compare);
	/* For comparison: order-0 same cpu */
	time_bench_loop(loops, 0, "single_cpu_page_alloc_put",
			NULL, time_single_cpu_page_alloc_put);
}

/* TODO: Need a super efficient way to transfer objects between two
 * CPUs, as the purpose is to isolate the cost the page allocator code
 * of touching the page.
 *
 * Idea(1): Could use ptr_ring as it can avoid the CPU
 *          producer/consumer head/tail memory bouncing.
 *
 * Idea(2): Do bulking into a queue, that don't have the
 *          producer/consumer head/tail memory bouncing problem, like
 *          ptr_ring.
 */
// Like skb_array_parallel.c
static int time_cross_cpu_ptr_ring(
	struct time_bench_record *rec, void *data)
{
	struct ptr_ring *queue = (struct ptr_ring*)data;
	//gfp_t gfp_mask = (GFP_ATOMIC | ___GFP_NORETRY);
	struct page *page, *npage;
	uint64_t loops_cnt = 0;
	int i;

	bool enq_CPU = false;

	/* Split CPU between enq/deq based on even/odd */
	if ((smp_processor_id() % 2)== 0)
		enq_CPU = true;

	//if (page_order) /* set: __GFP_COMP for compound pages */
	//	gfp_mask |= __GFP_COMP;

	/* Hack: use "step" to mark enq/deq, as "step" gets printed */
	rec->step = enq_CPU;

	/* Page to enqueue */
	// alloc_pages(gfp_mask, page_order);
	page = (struct page*) 43; /* fake pointer for baseline */

	if (queue == NULL) {
		pr_err("Need queue ptr as input\n");
		return 0;
	}
	/* loop count is limited to 32-bit due to div_u64_rem() use */
	if (((uint64_t)rec->loops * 2) >= ((1ULL<<32)-1)) {
		pr_err("Loop cnt too big will overflow 32-bit\n");
		return 0;
	}

	time_bench_start(rec);
	/** Loop to measure **/
	for (i = 0; i < rec->loops; i++) {

		if (enq_CPU) {
			/* enqueue side */
			if (ptr_ring_produce(queue, page) < 0) {
				pr_err("%s() WARN: enq fullq(CPU:%d) i:%d\n",
				       __func__, smp_processor_id(), i);
				goto finish_early;
			}
		} else {
			/* dequeue side */
			npage = ptr_ring_consume(queue);
			if (npage == NULL) {
				pr_err("%s() WARN: deq emptyq (CPU:%d) i:%d\n",
				       __func__, smp_processor_id(), i);
				goto finish_early;
			}
		}
		loops_cnt++;
		barrier(); /* compiler barrier */
	}
finish_early:
	time_bench_stop(rec, loops_cnt);

	return loops_cnt;
}


static int time_cross_cpu_page_alloc_put(
	struct time_bench_record *rec, void *data)
{
	struct ptr_ring *queue = (struct ptr_ring*)data;
//	gfp_t gfp_mask = (GFP_ATOMIC | ___GFP_NORETRY);
	gfp_t gfp_mask = (GFP_KERNEL);
	struct page *page, *npage;
	uint64_t loops_cnt = 0;
	int i;

	bool enq_CPU = false;

	/* Split CPU between enq/deq based on even/odd */
	if ((smp_processor_id() % 2)== 0)
		enq_CPU = true;

	if (page_order) /* set: __GFP_COMP for compound pages */
		gfp_mask |= __GFP_COMP;

	/* Hack: use "step" to mark enq/deq, as "step" gets printed */
	rec->step = enq_CPU;

	if (queue == NULL) {
		pr_err("Need queue ptr as input\n");
		return 0;
	}
	/* loop count is limited to 32-bit due to div_u64_rem() use */
	if (((uint64_t)rec->loops * 2) >= ((1ULL<<32)-1)) {
		pr_err("Loop cnt too big will overflow 32-bit\n");
		return 0;
	}

	time_bench_start(rec);
	/** Loop to measure **/
	for (i = 0; i < rec->loops; i++) {

		if (enq_CPU) {
			/* enqueue side */
			page = alloc_pages(gfp_mask, page_order);
			if (ptr_ring_produce(queue, page) < 0) {
				pr_err("%s() WARN: enq fullq(CPU:%d) i:%d\n",
				       __func__, smp_processor_id(), i);
				goto finish_early;
			}
		} else {
			/* dequeue side */
			npage = ptr_ring_consume(queue);
			if (npage == NULL) {
				pr_err("%s() WARN: deq emptyq (CPU:%d) i:%d\n",
				       __func__, smp_processor_id(), i);
				goto finish_early;
			}
			put_page(npage);
		}
		loops_cnt++;
		barrier(); /* compiler barrier */
	}
finish_early:
	time_bench_stop(rec, loops_cnt);

	return loops_cnt;
}


int run_parallel(const char *desc, uint32_t loops, const cpumask_t *cpumask,
		 int step, void *data,
		 int (*func)(struct time_bench_record *record, void *data)
	)
{
	struct time_bench_sync sync;
	struct time_bench_cpu *cpu_tasks;
	size_t size;

	/* Allocate records for every CPU */
	size = sizeof(*cpu_tasks) * num_possible_cpus();
	cpu_tasks = kzalloc(size, GFP_KERNEL);

	time_bench_run_concurrent(loops, step, data,
				  cpumask, &sync, cpu_tasks, func);
	time_bench_print_stats_cpumask(desc, cpu_tasks, cpumask);

	kfree(cpu_tasks);
	return 1;
}

bool init_queue(struct ptr_ring *queue, int q_size, int prefill,
		bool fake_ptr)
{
//	gfp_t gfp_mask = (GFP_ATOMIC | ___GFP_NORETRY);
	gfp_t gfp_mask = (GFP_KERNEL);
	struct page *page;
	int result, i;

	if (page_order) /* set: __GFP_COMP for compound pages */
		gfp_mask |= __GFP_COMP;

	result = ptr_ring_init(queue, q_size, GFP_KERNEL);
	if (result < 0) {
		pr_err("%s() err creating queue size:%d\n", __func__, q_size);
		return false;
	}

	if (fake_ptr)
		page = (struct page *) 42; /* Fake ptr */

	/*
	 *  Prefill with objects, in-order to keep enough distance
	 *  between producer and consumer, so the benchmark does not
	 *  run dry of objects to dequeue.
	 */
	for (i = 0; i < prefill; i++) {
		if (!fake_ptr) {
			page = alloc_pages(gfp_mask, page_order);
			if (unlikely(page == NULL)) {
				pr_err("%s() alloc cannot prefill:%d sz:%d\n",
				       __func__, prefill, q_size);
				return false;
			}
		}
		if (ptr_ring_produce(queue, page) < 0) {
			pr_err("%s() queue cannot prefill:%d sz:%d\n",
			       __func__, prefill, q_size);
			return false;
		}
	}

	return true;
}

void noinline run_bench_baseline_ptr_ring_cross_cpu(
	uint32_t loops, int q_size, int prefill)
{
	struct ptr_ring *queue;
	cpumask_t cpumask;

	run_or_return(bit_run_bench_ptr_ring_baseline);

	queue = kzalloc(sizeof(*queue), GFP_KERNEL);

	/* Restrict the CPUs to run on
	 */
	cpumask_clear(&cpumask);
	cpumask_set_cpu(0, &cpumask);
	cpumask_set_cpu(1, &cpumask);

	if (!init_queue(queue, q_size, prefill, true))
	    goto fail;

	run_parallel("baseline_ptr_ring_cross_cpu",
		     loops, &cpumask, 0, queue,
		     time_cross_cpu_ptr_ring);

fail:
	ptr_ring_cleanup(queue, NULL);
	kfree(queue);
}

void destructor_put_page(void *ptr)
{
	put_page(ptr);
}

void noinline run_bench_cross_cpu_page_alloc_put(
	uint32_t loops, int q_size, int prefill)
{
	struct ptr_ring *queue;
	cpumask_t cpumask;

	run_or_return(bit_run_bench_cross_cpu_page_alloc_put);

	queue = kzalloc(sizeof(*queue), GFP_KERNEL);

	/* Restrict the CPUs to run on
	 */
	cpumask_clear(&cpumask);
	cpumask_set_cpu(0, &cpumask);
	cpumask_set_cpu(1, &cpumask);

	if (!init_queue(queue, q_size, prefill, false))
	    goto fail;

	run_parallel("cross_cpu_page_alloc_put",
		     loops, &cpumask, 0, queue,
		     time_cross_cpu_page_alloc_put);

fail:
	ptr_ring_cleanup(queue, destructor_put_page);
	kfree(queue);
}

int run_timing_tests(void)
{
	/* ADJUST: These likely need some adjustments on different
	 * systems, else the tests likely cannot "complete", because
	 * the CPUs catchup to each-other.
	 *
	 * The benchmark will stop as soon as the CPUs catchup, either
	 * when the queue is full, or the queue is empty.
	 *
	 * If the test does not complete the number of "loops", then
	 * the results are still showed, but a WARNing is printed
	 * indicating how many interations were completed.  Thus, you
	 * can judge if the results are valid.
	 */
	int prefill = 8000;
	int q_size = 32000;

	run_bench_order0_compare(loops);

	run_bench_baseline_ptr_ring_cross_cpu(loops, q_size, prefill);
	run_bench_cross_cpu_page_alloc_put(loops, q_size, prefill);

	return 0;
}

static int __init page_bench05_module_init(void)
{
	if (verbose)
		pr_info("Loaded (using page_order:%d)\n", page_order);

	if (run_timing_tests() < 0) {
		return -ECANCELED;
	}

	return 0;
}
module_init(page_bench05_module_init);

static void __exit page_bench05_module_exit(void)
{
	if (verbose)
		pr_info("Unloaded\n");
}
module_exit(page_bench05_module_exit);

MODULE_DESCRIPTION("Benchmarking page alloactor: Cross CPU cost");
MODULE_AUTHOR("Jesper Dangaard Brouer <netoptimizer@brouer.com>");
MODULE_LICENSE("GPL");
