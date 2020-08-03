#ifndef _LINUX_ALF_QUEUE_H
#define _LINUX_ALF_QUEUE_H
/* linux/alf_queue.h
 *
 * ALF: Array-based Lock-Free queue
 *
 * Queue properties
 *  - Array based for cache-line optimization
 *  - Bounded by the array size
 *  - FIFO Producer/Consumer queue, no queue traversal supported
 *  - Very fast
 *  - Designed as a queue for pointers to objects
 *  - Bulk enqueue and dequeue support
 *  - Supports combinations of Multi and Single Producer/Consumer
 *
 * Copyright (C) 2014, Red Hat, Inc.,
 *  by Jesper Dangaard Brouer and Hannes Frederic Sowa
 *  for licensing details see kernel-base/COPYING
 */
#include <linux/compiler.h>
#include <linux/kernel.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
static __always_inline void __read_once_size(const volatile void *p, void *res, int size)
{
	switch (size) {
	case 1: *(__u8 *)res = *(volatile __u8 *)p; break;
	case 2: *(__u16 *)res = *(volatile __u16 *)p; break;
	case 4: *(__u32 *)res = *(volatile __u32 *)p; break;
#ifdef CONFIG_64BIT
	case 8: *(__u64 *)res = *(volatile __u64 *)p; break;
#endif
	default:
		barrier();
		__builtin_memcpy((void *)res, (const void *)p, size);
		data_access_exceeds_word_size();
		barrier();
	}
}

static __always_inline void __write_once_size(volatile void *p, void *res, int size)
{
	switch (size) {
	case 1: *(volatile __u8 *)p = *(__u8 *)res; break;
	case 2: *(volatile __u16 *)p = *(__u16 *)res; break;
	case 4: *(volatile __u32 *)p = *(__u32 *)res; break;
#ifdef CONFIG_64BIT
	case 8: *(volatile __u64 *)p = *(__u64 *)res; break;
#endif
	default:
		barrier();
		__builtin_memcpy((void *)p, (const void *)res, size);
		data_access_exceeds_word_size();
		barrier();
	}
}
#endif

#define READ_ONCE(x) \
        ({ union { typeof(x) __val; char __c[1]; } __u; __read_once_size(&(x), __u.__c, sizeof(x)); __u.__val; })

#define WRITE_ONCE(x, val) \
        ({ typeof(x) __val = (val); __write_once_size(&(x), &__val, sizeof(__val)); __val; })

struct alf_actor {
	u32 head;
	u32 tail;
};

struct alf_queue {
	u32 size;
	u32 mask;
	u32 flags;
	struct alf_actor producer ____cacheline_aligned_in_smp;
	struct alf_actor consumer ____cacheline_aligned_in_smp;
	void *ring[0] ____cacheline_aligned_in_smp;
};

struct alf_queue *alf_queue_alloc(u32 size, gfp_t gfp);
void		  alf_queue_free(struct alf_queue *q);

/* Helpers for LOAD and STORE of elements, have been split-out because:
 *  1. They can be reused for both "Single" and "Multi" variants
 *  2. Allow us to experiment with (pipeline) optimizations in this area.
 */
/* Only a single of these helpers will survive upstream submission */
#include "alf_queue_helpers.h"
#define __helper_alf_enqueue_store __helper_alf_enqueue_store_unroll
#define __helper_alf_dequeue_load  __helper_alf_dequeue_load_unroll

/* Main Multi-Producer ENQUEUE
 *
 * Even-though current API have a "fixed" semantics of aborting if it
 * cannot enqueue the full bulk size.  Users of this API should check
 * on the returned number of enqueue elements match, to verify enqueue
 * was successful.  This allow us to introduce a "variable" enqueue
 * scheme later.
 *
 * Not preemption safe. Multiple CPUs can enqueue elements, but the
 * same CPU is not allowed to be preempted and access the same
 * queue. Due to how the tail is updated, this can result in a soft
 * lock-up. (Same goes for alf_mc_dequeue).
 */
static inline int
alf_mp_enqueue(const u32 n;
	       struct alf_queue *q, void *ptr[n], const u32 n)
{
	u32 p_head, p_next, c_tail, space;

	/* Reserve part of the array for enqueue STORE/WRITE */
	do {
		p_head = READ_ONCE(q->producer.head);
		c_tail = READ_ONCE(q->consumer.tail);/* as smp_load_aquire */

		space = q->size + c_tail - p_head;
		if (n > space)
			return 0;

		p_next = p_head + n;
	}
	while (unlikely(cmpxchg(&q->producer.head, p_head, p_next) != p_head));
	/* The memory barrier of smp_load_acquire(&q->consumer.tail)
	 * is satisfied by cmpxchg implicit full memory barrier
	 */

	/* STORE the elems into the queue array */
	__helper_alf_enqueue_store(p_head, q, ptr, n);
	smp_wmb(); /* Write-Memory-Barrier matching dequeue LOADs */

	/* Wait for other concurrent preceding enqueues not yet done,
	 * this part make us none-wait-free and could be problematic
	 * in case of congestion with many CPUs
	 */
	while (unlikely(READ_ONCE(q->producer.tail) != p_head))
		cpu_relax();
	/* Mark this enq done and avail for consumption */
	WRITE_ONCE(q->producer.tail, p_next);

	return n;
}

/* Main Multi-Consumer DEQUEUE */
static inline int
alf_mc_dequeue(const u32 n;
	       struct alf_queue *q, void *ptr[n], const u32 n)
{
	u32 c_head, c_next, p_tail, elems;

	/* Reserve part of the array for dequeue LOAD/READ */
	do {
		c_head = READ_ONCE(q->consumer.head);
		p_tail = READ_ONCE(q->producer.tail);

		elems = p_tail - c_head;

		if (elems == 0)
			return 0;
		else
			elems = min(elems, n);

		c_next = c_head + elems;
	}
	while (unlikely(cmpxchg(&q->consumer.head, c_head, c_next) != c_head));

	/* LOAD the elems from the queue array.
	 *   We don't need a smb_rmb() Read-Memory-Barrier here because
	 *   the above cmpxchg is an implied full Memory-Barrier.
	 */
	__helper_alf_dequeue_load(c_head, q, ptr, elems);

	/* Wait for other concurrent preceding dequeues not yet done */
	while (unlikely(READ_ONCE(q->consumer.tail) != c_head))
		cpu_relax();
	/* Mark this deq done and avail for producers */
	smp_store_release(&q->consumer.tail, c_next);
	/* Archs with weak Memory Ordering need a memory barrier
	 * (store_release) here.  As the STORE to q->consumer.tail,
	 * must happen after the dequeue LOADs.  Paired with enqueue
	 * implicit full-MB in cmpxchg.
	 */

	return elems;
}

/* #define ASSERT_DEBUG_SPSC 1 */
#ifndef ASSERT_DEBUG_SPSC
#define ASSERT(x) do { } while (0)
#else
#define ASSERT(x)							\
	do {								\
		if (unlikely(!(x))) {					\
			pr_crit("Assertion failed %s:%d: \"%s\"\n",	\
				__FILE__, __LINE__, #x);		\
			BUG();						\
		}							\
	} while (0)
#endif

/* Main SINGLE Producer ENQUEUE
 *  caller MUST make sure preemption is disabled
 */
static inline int
alf_sp_enqueue(const u32 n;
	       struct alf_queue *q, void *ptr[n], const u32 n)
{
	u32 p_head, p_next, c_tail, space;

	/* Reserve part of the array for enqueue STORE/WRITE */
	p_head = q->producer.head;
	smp_rmb(); /* for consumer.tail write, making sure deq loads are done */
	c_tail = READ_ONCE(q->consumer.tail);

	space = q->size + c_tail - p_head;
	if (n > space)
		return 0;

	p_next = p_head + n;
	ASSERT(READ_ONCE(q->producer.head) == p_head);
	q->producer.head = p_next;

	/* STORE the elems into the queue array */
	__helper_alf_enqueue_store(p_head, q, ptr, n);
	smp_wmb(); /* Write-Memory-Barrier matching dequeue LOADs */

	/* Assert no other CPU (or same CPU via preemption) changed queue */
	ASSERT(READ_ONCE(q->producer.tail) == p_head);

	/* Mark this enq done and avail for consumption */
	WRITE_ONCE(q->producer.tail, p_next);

	return n;
}

/* Main SINGLE Consumer DEQUEUE
 *  caller MUST make sure preemption is disabled
 */
static inline int
alf_sc_dequeue(const u32 n;
	       struct alf_queue *q, void *ptr[n], const u32 n)
{
	u32 c_head, c_next, p_tail, elems;

	/* Reserve part of the array for dequeue LOAD/READ */
	c_head = q->consumer.head;
	p_tail = READ_ONCE(q->producer.tail);

	elems = p_tail - c_head;

	if (elems == 0)
		return 0;
	else
		elems = min(elems, n);

	c_next = c_head + elems;
	ASSERT(READ_ONCE(q->consumer.head) == c_head);
	q->consumer.head = c_next;

	smp_rmb(); /* Read-Memory-Barrier matching enq STOREs */
	__helper_alf_dequeue_load(c_head, q, ptr, elems);

	/* Archs with weak Memory Ordering need a memory barrier here.
	 * As the STORE to q->consumer.tail, must happen after the
	 * dequeue LOADs. Dequeue LOADs have a dependent STORE into
	 * ptr, thus a smp_wmb() is enough.
	 */
	smp_wmb();

	/* Assert no other CPU (or same CPU via preemption) changed queue */
	ASSERT(READ_ONCE(q->consumer.tail) == c_head);

	/* Mark this deq done and avail for producers */
	WRITE_ONCE(q->consumer.tail, c_next);

	return elems;
}

static inline bool
alf_queue_empty(struct alf_queue *q)
{
	u32 c_tail = READ_ONCE(q->consumer.tail);
	u32 p_tail = READ_ONCE(q->producer.tail);

	/* The empty (and initial state) is when consumer have reached
	 * up with producer.
	 *
	 * DOUBLE-CHECK: Should we use producer.head, as this indicate
	 * a producer is in-progress(?)
	 */
	return c_tail == p_tail;
}

static inline int
alf_queue_count(struct alf_queue *q)
{
	u32 c_head = READ_ONCE(q->consumer.head);
	u32 p_tail = READ_ONCE(q->producer.tail);
	u32 elems;

	/* Due to u32 arithmetic the values are implicitly
	 * masked/modulo 32-bit, thus saving one mask operation
	 */
	elems = p_tail - c_head;
	/* Thus, same as:
	 *  elems = (p_tail - c_head) & q->mask;
	 */
	return elems;
}

static inline int
alf_queue_avail_space(struct alf_queue *q)
{
	u32 p_head = READ_ONCE(q->producer.head);
	u32 c_tail = READ_ONCE(q->consumer.tail);
	u32 space;

	/* The max avail space is q->size and
	 * the empty state is when (consumer == producer)
	 */

	/* Due to u32 arithmetic the values are implicitly
	 * masked/modulo 32-bit, thus saving one mask operation
	 */
	space = q->size + c_tail - p_head;
	/* Thus, same as:
	 *  space = (q->size + c_tail - p_head) & q->mask;
	 */
	return space;
}

int alf_queue_test_module_init(void);

#endif /* _LINUX_ALF_QUEUE_H */
