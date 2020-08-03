/*
 * Valet: Efficient Orchestration of Host and Remote Shared Memory
 *        for Memory IntensiveWorkloads
 * Copyright 2020 Georgia Institute of Technology.
 */
#ifndef VALET_MEMPOOL_H
#define VALET_MEMPOOL_H

#include <linux/wait.h>

#include "valet_drv.h"

// 1GB step increasing for mempool_resize
#define RESIZING_UNIT_IN_PAGES 262144 //1GB

/* Default mempool min size: 5% of Free RAM */
static size_t default_mempool_min = 5;
/* Default mempool max size: 40% of Free  RAM */
static size_t default_mempool_max = 40;

static size_t default_mempool_shrink_perc = 15;
static size_t default_mempool_expand_perc = 30;

enum mempool_state {
        MEMP_IDLE = 1,
        MEMP_SHRINK,
        MEMP_EXPAND
};
struct local_page_list{
        atomic_t ref_count;
        size_t start_index;
        size_t len;
	struct tree_entry *batch_list[RDMA_WR_BUF_LEN];
        struct list_head list;
#ifdef DISKBACKUP
	struct bio *cloned_bio;
#endif
};

typedef struct valet_mempool_s {
	spinlock_t lock;
	spinlock_t slock;
	spinlock_t rlock;
	spinlock_t flock;

	long cap_nr;		/* capacity. nr of elements at *elements */
	long new_cap_nr;	/* new capacity. nr of elements at *elements */
	long curr_nr;		/* curr nr of elements at *elements */
	long used_nr;		/* used nr of elements at *elements */
	long threshold;

	void **elements;

	int init_done;

        long min_pool_pages;
        long max_pool_pages;
	long threshold_page_shrink;
        long threshold_page_expand;

	struct alf_queue *sending_list;
    	struct alf_queue *reclaim_list;

        enum mempool_state state;
	struct task_struct *mempool_thread;
} valet_mempool_t;

static valet_mempool_t valet_page_pool;

extern int mempool_init();
extern int valet_mempool_create(valet_mempool_t *pool, long cap_nr);
extern int valet_mempool_resize(valet_mempool_t *pool, long new_cap_nr);
extern void valet_mempool_destroy(valet_mempool_t *pool);

extern void * valet_mempool_alloc(valet_mempool_t *pool);
extern void valet_mempool_free(void *element, valet_mempool_t *pool);
extern void * valet_alloc();
extern void valet_free(void *element);

extern void * valet_mempool_reclaim(valet_mempool_t *pool);

extern struct local_page_list* get_free_item();
extern int put_free_item(struct local_page_list *tmp);
extern int put_sending_index(struct local_page_list *tmp);
extern int get_sending_index(struct local_page_list **tmp);
extern int put_reclaimable_index(void *tmp);
extern int get_reclaimable_index(void **tmp);

extern int is_mempool_init_done();


#endif
