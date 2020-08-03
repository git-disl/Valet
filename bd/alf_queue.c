/*
 * lib/alf_queue.c
 *
 * ALF: Array-based Lock-Free queue
 *  - Main implementation in: include/linux/alf_queue.h
 *
 * Copyright (C) 2014, Red Hat, Inc.,
 *  by Jesper Dangaard Brouer and Hannes Frederic Sowa
 *  for licensing details see kernel-base/COPYING
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/slab.h> /* kzalloc */
#include "alf_queue.h"
#include <linux/log2.h>

struct alf_queue *alf_queue_alloc(u32 size, gfp_t gfp)
{
	struct alf_queue *q;
	size_t mem_size;

	/* The ring array is allocated together with the queue struct */
	mem_size = size * sizeof(void *) + sizeof(struct alf_queue);
	q = vzalloc(mem_size);
	if (!q)
		return ERR_PTR(-ENOMEM);

	q->size = size;
	q->mask = size - 1;

	return q;
}
EXPORT_SYMBOL_GPL(alf_queue_alloc);

void alf_queue_free(struct alf_queue *q)
{
	vfree(q);
}
EXPORT_SYMBOL_GPL(alf_queue_free);

MODULE_DESCRIPTION("ALF: Array-based Lock-Free queue");
MODULE_AUTHOR("Jesper Dangaard Brouer <netoptimizer@brouer.com>");
MODULE_LICENSE("GPL");
