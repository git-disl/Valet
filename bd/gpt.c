/*
 * Valet: Efficient Orchestration of Host and Remote Shared Memory
 *        for Memory IntensiveWorkloads
 * Copyright 2020 Georgia Institute of Technology.
 *
 * Copyright (C) 2007 Nick Piggin
 * Copyright (C) 2007 Novell Inc.
 *
 * Parts derived from drivers/block/rd.c, and drivers/block/loop.c, copyright
 * of their respective owners.
 * drivers/block/brd.c
 */

#include "debug.h"

#include "valet_drv.h"
#include "gpt.h"
#include "valet_mempool.h"
#include "rdmabox.h"

int test_flag(struct tree_entry *entry,enum entry_flags flag)
{
        return entry->flags & BIT(flag);
}
        
int set_flag(struct tree_entry *entry,enum entry_flags flag)
{
        entry->flags |= BIT(flag);
}
        
int clear_flag(struct tree_entry *entry,enum entry_flags flag)
{
        entry->flags &= ~BIT(flag);
}

struct tree_entry *gpt_lookup_page(struct gpt_device *brd, size_t index)
{
	struct tree_entry *entry;

	rcu_read_lock();
	entry = radix_tree_lookup(&brd->gpt_pages, index);
	rcu_read_unlock();

	return entry; 
}

struct tree_entry *gpt_lookup(size_t index)
{
    return gpt_lookup_page(&brd,index);
}

struct tree_entry* gpt_alloc_entry(struct gpt_device *brd, size_t index)
{
	struct tree_entry *entry;
	unsigned long flags;

        entry = (struct tree_entry *)kmalloc(sizeof(struct tree_entry),GFP_ATOMIC); 
        if(!entry){
	    printk("gpt[%s]: allocate entry fail \n", __func__);
	    return NULL;
        }
        entry->page = valet_alloc();
	if(!entry->page){
            put_cpu();
            kfree(entry);
	    return NULL;
	}
	entry->len=VALET_PAGE_SIZE;
	entry->flags=0;

	return entry;
}

struct tree_entry* gpt_alloc(size_t index)
{
    return gpt_alloc_entry(&brd, index);
}

struct tree_entry* gpt_insert_page(struct gpt_device *brd, struct tree_entry *entry, size_t index)
{
        unsigned long flags;

        spin_lock_irqsave(&brd->gpt_lock, flags);
	entry = radix_tree_lookup(&brd->gpt_pages, index);
        spin_unlock_irqrestore(&brd->gpt_lock, flags);
        if(!entry){
            entry = gpt_alloc_entry(brd, index);
            if(!entry){
	        return NULL;
            }
	    atomic_set(&entry->ref_count,0);
        }else{
            if(!entry->page){
                entry->page = valet_alloc();
		if(!entry->page)
		    return NULL;
            }
	    atomic_inc(&entry->ref_count);
            set_flag(entry,UPDATING);
	    return entry;
        }
       
       	if (radix_tree_preload(GFP_NOIO)) {
		valet_free(entry->page);
	        kfree(entry);
		printk("gpt[%s]: radix_tree_preload fail \n", __func__);
		return NULL;
	}

        spin_lock_irqsave(&brd->gpt_lock, flags);
	if (radix_tree_insert(&brd->gpt_pages, index, entry)) {
		printk("gpt[%s]: radix_tree_insert fail. entry exists in radix_tree. \n", __func__);
		panic("radix_tree_insert fail");
	}
        spin_unlock_irqrestore(&brd->gpt_lock, flags);
	radix_tree_preload_end();

	return entry;
}

struct tree_entry* gpt_insert(struct tree_entry *entry, size_t index)
{
    return gpt_insert_page(&brd, entry, index);
}

void gpt_free_entry(struct gpt_device *brd, size_t index)
{
	struct tree_entry *entry;
	unsigned long flags;

	spin_lock_irqsave(&brd->gpt_lock, flags);
	entry = radix_tree_delete(&brd->gpt_pages, index);
	spin_unlock_irqrestore(&brd->gpt_lock, flags);
	if (entry){
		valet_free(entry->page);
		kfree(entry);
	}
}

void gpt_free(size_t index)
{
	gpt_free_entry(&brd, index);
}

struct page * gpt_remove_page(struct gpt_device *brd, size_t index)
{
	struct page *page;
	struct tree_entry *entry;
	unsigned long flags;

        spin_lock_irqsave(&brd->gpt_lock, flags);
	entry = radix_tree_lookup(&brd->gpt_pages, index);
	if(!entry){
            spin_unlock_irqrestore(&brd->gpt_lock,flags);
	    return NULL;
	}
	page = entry->page;
        entry->page = NULL;
        entry->len = 0;
        spin_unlock_irqrestore(&brd->gpt_lock,flags);

	return page;
}

struct page * gpt_remove(size_t index)
{
	return gpt_remove_page(&brd, index);
}

/*
 * Free all backing store pages and radix tree. This must only be called when
 * there are no other users of the device.
 */
#define FREE_BATCH 16
static void gpt_free_pages(struct gpt_device *brd)
{
	unsigned long pos = 0;
	struct tree_entry *entries[FREE_BATCH];
	int nr_entries;

	do {
		int i;

		nr_entries = radix_tree_gang_lookup(&brd->gpt_pages,
				(void **)entries, pos, FREE_BATCH);

		for (i = 0; i < nr_entries; i++) {
			void *ret;

			BUG_ON(entries[i]->page->index < pos);
			pos = entries[i]->page->index;
			ret = radix_tree_delete(&brd->gpt_pages, pos);
			BUG_ON(!ret || ret != entries[i]);
			// real delete
			__free_page(entries[i]->page);
			kfree(entries[i]);
		}

		pos++;

		/*
		 * This assumes radix_tree_gang_lookup always returns as
		 * many pages as possible. If the radix-tree code changes,
		 * so will this have to.
		 */
	} while (nr_entries == FREE_BATCH);
}

struct bio * create_bio_copy(struct bio *src)
{
   int	status;
   struct bio *dst;
	if ( !(dst = bio_clone(src, GFP_KERNEL)) )
		{
		printk("bio_clone_bioset() -> NULL\n");

		return NULL;
		}

	if ( status = bio_alloc_pages(dst , GFP_KERNEL))
		{
		printk("bio_alloc_pages() -> %d\n", status);

		return NULL;
		}
  return dst;
}

int write_to_brd(struct gpt_device *brd, struct request *req, struct valet_session *valet_sess)
{
  int ret;
  size_t start_index;
  size_t i,j;
  unsigned char *src, *cmem;
  unsigned char *buffer;
  struct bio *tmp = req->bio;
  struct local_page_list *tmp_item;

#ifdef DISKBACKUP
  struct bio *cloned_bio;
  cloned_bio = create_bio_copy(req->bio);
  if (unlikely(!cloned_bio)) {
    printk("gpt[%s]: fail to clone bio \n",__func__);
    return 1;
  }
#endif

  if (unlikely(!brd->init_done)) {
    printk("gpt[%s]: brd not init done yet \n",__func__);
    return 1;
  }
  start_index = blk_rq_pos(req) >> SECTORS_PER_PAGE_SHIFT; // 3

  get_cpu();
repeat:
  tmp_item = get_free_item();
  if(unlikely(!tmp_item)){
	goto repeat;
  }
  tmp_item->start_index = start_index;
  tmp_item->len = req->nr_phys_segments;
#ifdef DISKBACKUP
  tmp_item->cloned_bio = cloned_bio;
#endif

  for (i=0; i < req->nr_phys_segments;i++){
        struct tree_entry *entry;
        entry = gpt_insert_page(brd, entry, start_index+i);
	tmp_item->batch_list[i] = entry;
  }

  for (i=0; i < req->nr_phys_segments;i++){
    struct tree_entry *entry;
    entry = tmp_item->batch_list[i];
    buffer = bio_data(tmp);

    cmem = kmap_atomic(entry->page);
    memcpy(cmem, buffer, VALET_PAGE_SIZE);
    kunmap_atomic(cmem);

    entry->len = VALET_PAGE_SIZE;

    tmp = tmp->bi_next;
  }

send_again:
  ret = put_sending_index(tmp_item);
  if(ret){
	printk("gpt[%s]: Fail to put sending index. retry \n", __func__);
	goto send_again;
  }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 18, 0)
  blk_mq_end_request(req, 0);
#else
  blk_mq_end_io(req, 0);
#endif

  //remote_sender_fn(valet_sess);

  put_cpu();
  return 0;
}

int radix_write(struct valet_session *valet_sess, struct request *req)
{
   return write_to_brd(&brd, req, valet_sess);
}

int read_from_brd(struct gpt_device *brd, struct request *req, struct valet_session *valet_sess)
{
  int i, ret;
  size_t index;
  struct tree_entry *entry;
  unsigned char *cmem;
  struct local_page_list *tmp_item;
  int cpu=get_cpu();

  if (unlikely(!brd->init_done)) {
    printk("gpt[%s]: gpt not init done yet \n", __func__);
    return 1;
  }

  index = blk_rq_pos(req) >> SECTORS_PER_PAGE_SHIFT;

  entry = radix_tree_lookup(&brd->gpt_pages, index);
  if (entry && entry->page) {
      cmem = kmap_atomic(entry->page);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
      memcpy(bio_data(req->bio), cmem, entry->len);
#else
      memcpy(req->buffer, cmem, entry->len);
#endif
      kunmap_atomic(cmem);

      goto endio;
  } 

   // if local miss, request remote read
  ret = request_read(valet_sess,req);
  if(ret){
#ifdef DISKBACKUP
       // read from disk
       disk_write_bio_clone(req);
#else
       goto endio;
#endif
  }
  goto out;

endio:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 18, 0)
  blk_mq_end_request(req, 0);
#else
  blk_mq_end_io(req, 0);
#endif

out:
  put_cpu();
  return 0;
}

int radix_read(struct valet_session *valet_sess, struct request *req)
{
   return read_from_brd(&brd, req, valet_sess);
}

int gpt_init(u64 size)
{
        printk("gpt[%s]: gpt init start \n", __func__);
        brd.init_done = 0;

	spin_lock_init(&brd.gpt_lock);
	INIT_RADIX_TREE(&brd.gpt_pages, GFP_ATOMIC);

        brd.init_done = 1;

        printk("gpt[%s]: gpt init done \n", __func__);
	return 0;
}

void gpt_destroy(struct gpt_device *brd)
{
	gpt_free_pages(brd);
	kfree(brd);
}
