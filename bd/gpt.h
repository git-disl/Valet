/*
 * Valet: Efficient Orchestration of Host and Remote Shared Memory
 *        for Memory IntensiveWorkloads
 * Copyright 2020 Georgia Institute of Technology.
 *
 */
#ifndef GPT_H
#define GPT_H

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/major.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/highmem.h>
#include <linux/mutex.h>
#include <linux/radix-tree.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <asm/uaccess.h>

#include "rdmabox.h"

#define SECTOR_SIZE             512
#define SECTOR_SHIFT            ilog2(SECTOR_SIZE)
#define SECTORS_PER_PAGE_SHIFT  (PAGE_SHIFT - SECTOR_SHIFT)
#define SECTORS_PER_PAGE        (1 << SECTORS_PER_PAGE_SHIFT)

enum entry_flags {
        UPDATING,
        RECLAIMABLE,
        RECLAIMED,
};

struct tree_entry {
	size_t index;
	struct page *page;
	u16 len;
	u8 flags;
	atomic_t ref_count;
};

struct gpt_device {
	struct mutex init_lock;
        spinlock_t gpt_lock;
        struct radix_tree_root gpt_pages;
	int init_done;
};

static struct gpt_device brd;
extern int test_flag(struct tree_entry *entry, enum entry_flags flag);
extern int set_flag(struct tree_entry *entry, enum entry_flags flag);
extern int clear_flag(struct tree_entry *entry, enum entry_flags flag);

int radix_read(struct valet_session *valet_sess, struct request *req);
int radix_write(struct valet_session *valet_sess, struct request *req);

extern struct tree_entry *gpt_insert(struct tree_entry *entry, size_t index);
extern struct tree_entry *gpt_lookup(size_t index);
extern struct tree_entry* gpt_alloc(size_t index);
extern void gpt_free(size_t index);
extern struct page * gpt_remove(size_t index);

extern int gpt_init(u64 size);
extern void gpt_destroy(struct gpt_device *brd);
struct bio * create_bio_copy(struct bio *src);

#endif
