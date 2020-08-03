/*
 * Valet: Efficient Orchestration of Host and Remote Shared Memory
 *        for Memory IntensiveWorkloads
 * Copyright 2020 Georgia Institute of Technology.
 *
 * Stackbd
 * Copyright 2014 Oren Kishon
 * https://github.com/OrenKishon/stackbd
 *
 * Copyright (c) 2013 Mellanox Technologies��. All rights reserved.
 *
 * This software is available to you under a choice of one of two licenses.
 * You may choose to be licensed under the terms of the GNU General Public
 * License (GPL) Version 2, available from the file COPYING in the main
 * directory of this source tree, or the Mellanox Technologies�� BSD license
 * below:
 *
 *      - Redistribution and use in source and binary forms, with or without
 *        modification, are permitted provided that the following conditions
 *        are met:
 *
 *      - Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 *      - Neither the name of the Mellanox Technologies�� nor the names of its
 *        contributors may be used to endorse or promote products derived from
 *        this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "debug.h"

#include "valet_drv.h"
#include "rdmabox.h"
#include "diskbox.h"
#include "gpt.h"
#include "valet_mempool.h"
#include <linux/smp.h>

/* from Infiniswap*/
/* lookup_bdev patch: https://www.redhat.com/archives/dm-devel/2016-April/msg00372.html */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
#define HAVE_LOOKUP_BDEV_PATCH
#endif
#ifdef HAVE_LOOKUP_BDEV_PATCH
#define LOOKUP_BDEV(x) lookup_bdev(x, 0)
#else
#define LOOKUP_BDEV(x) lookup_bdev(x)
#endif

void valet_stackbd_end_io(struct bio *bio, int err)
{
  struct request *req = (struct request *)ptr_from_uint64(bio->bi_private);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 18, 0)
  blk_mq_end_request(req, err);
#else
  blk_mq_end_io(req, err);
#endif
}

void valet_stackbd_end_io_postwork(struct bio *bio, int err)
{
  struct rdma_ctx *ctx=NULL;
  struct local_page_list *sending_item=NULL;
  size_t i;
  int ret;
#ifdef DISKBACKUP
  int num_replica = NUM_REPLICA+1;
#else
  int num_replica = NUM_REPLICA;
#endif

  ctx = (struct rdma_ctx *)ptr_from_uint64(bio->bi_private);
  if (ctx == NULL || ctx->rb == NULL){
    printk("rdmabox[%s]: ctx is null or ctx->rb is null  \n",__func__);
    return;
  }

  sending_item = ctx->sending_item;
  if (unlikely(!sending_item)){
    printk("rdmabox[%s]: sending item is null  \n",__func__);
    return;
  }

  atomic_inc(&sending_item->ref_count);

  if(atomic_read(&ctx->sending_item->ref_count) < num_replica )
  {
     return ;
  }
  else{
      for (i=0; i < sending_item->len;i++){
          struct tree_entry *entry;
          entry = sending_item->batch_list[i];
          atomic_dec(&entry->ref_count);
          if(atomic_read(&entry->ref_count)<=0){
              clear_flag(entry,UPDATING);
              atomic_set(&entry->ref_count,0);
          }

          if(!test_flag(entry,RECLAIMABLE)){
              set_flag(entry,RECLAIMABLE);
              ret = put_reclaimable_index(entry);
              if(ret)
                  printk("rdmabox[%s]: fail to put reclaim list \n",__func__);
          }
       }//for
       put_free_item(sending_item);

       ctx->sending_item = NULL;
       ctx->rb_index = -1;
       ctx->rb = NULL;
       atomic_set(&ctx->in_flight, CTX_IDLE);
       put_rdma_freepool(&ctx->cb->valet_sess->rdma_write_freepool,ctx);
  }//else
}

static void stackbd_io_fn(struct bio *bio)
{
#if LINUX_VERSION_CODE <= KERNEL_VERSION(3, 14, 0)
  bio->bi_bdev = stackbd.bdev_raw;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
  trace_block_bio_remap(bdev_get_queue(stackbd.bdev_raw), bio, bio_dev(bio), 
#else
  trace_block_bio_remap(bdev_get_queue(stackbd.bdev_raw), bio, bio->bi_bdev->bd_dev, 
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0)
      bio->bi_iter.bi_sector);
#else
      bio->bi_sector);
#endif 

  generic_make_request(bio);
}
static int stackbd_threadfn(void *data)
{
  struct bio *bio;

  set_user_nice(current, -20);
  while (!kthread_should_stop())
  {
    wait_event_interruptible(req_event, kthread_should_stop() ||
	!bio_list_empty(&stackbd.bio_list));
    spin_lock_irq(&stackbd.lock);
    if (bio_list_empty(&stackbd.bio_list))
    {
      spin_unlock_irq(&stackbd.lock);
      continue;
    }
    bio = bio_list_pop(&stackbd.bio_list);
    spin_unlock_irq(&stackbd.lock);
    stackbd_io_fn(bio);
  }
  return 0;
}

void _stackbd_make_request_bio(struct bio *bio)
{
  spin_lock_irq(&stackbd.lock);
  if (!stackbd.bdev_raw)
  {
    goto abort;
  }
  if (!stackbd.is_active)
  {
    goto abort;
  }
  bio->bi_end_io = (bio_end_io_t*)valet_stackbd_end_io_postwork;
  bio_list_add(&stackbd.bio_list, bio);

  wake_up(&req_event);
  spin_unlock_irq(&stackbd.lock);
  return;
abort:
  spin_unlock_irq(&stackbd.lock);
  printk("diskbox[%s]: <%p> Abort request\n",__func__, bio);
  bio_io_error(bio);
}

void disk_write(struct rdma_ctx *ctx, struct bio *cloned_bio, unsigned int io_size, size_t start_index)
{
    struct page *pg = NULL;
    pg = virt_to_page(ctx->rdma_buf);
    cloned_bio->bi_sector = start_index  << SECTORS_PER_PAGE_SHIFT;
    cloned_bio->bi_io_vec->bv_page = pg;
    cloned_bio->bi_io_vec->bv_len = io_size;
    cloned_bio->bi_io_vec->bv_offset = 0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 14, 0)
    cloned_bio->bi_iter.bi_size = io_size;
#else
    cloned_bio->bi_size = io_size;
#endif
    cloned_bio->bi_private = uint64_from_ptr(ctx);
    _stackbd_make_request_bio(cloned_bio);
}

void _stackbd_make_request_bio_clone(struct request_queue *q, struct request *req)
{
  struct bio *bio = NULL;
  struct bio *b = req->bio;
  int i;
  int len = req->nr_phys_segments;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
  struct bio_set *io_bio_set;
#endif

  if (!stackbd.bdev_raw)
  {
    goto abort;
  }
  if (!stackbd.is_active)
  {
    goto abort;
  }
  for (i=0; i<len -1; i++){
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
    bio = bio_clone_fast(b, GFP_ATOMIC, io_bio_set);
#else
    bio = bio_clone(b, GFP_ATOMIC);
#endif
    spin_lock_irq(&stackbd.lock);
    bio_list_add(&stackbd.bio_list, bio);
    spin_unlock_irq(&stackbd.lock);
    b = b->bi_next;
  }
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
  bio = bio_clone_fast(b, GFP_ATOMIC, io_bio_set);
#else
  bio = bio_clone(b, GFP_ATOMIC);
#endif
  bio->bi_end_io = (bio_end_io_t*)valet_stackbd_end_io;
  bio->bi_private = uint64_from_ptr(req);
  spin_lock_irq(&stackbd.lock);
  bio_list_add(&stackbd.bio_list, bio);
  wake_up(&req_event);
  spin_unlock_irq(&stackbd.lock);
  return;
abort:
  bio_io_error(b);
  printk("diskbox[%s]: <%p> Abort request\n\n",__func__, bio);
}

void disk_write_bio_clone(struct request *req)
{
  _stackbd_make_request_bio_clone(stackbd.queue, req);
}

// from original stackbd
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
blk_qc_t stackbd_make_request(struct request_queue *q, struct bio *bio)
#else
void stackbd_make_request(struct request_queue *q, struct bio *bio)
#endif
{
  if (!stackbd.bdev_raw)
  {
    goto abort;
  }
  if (!stackbd.is_active)
  {
    goto abort;
  }
  spin_lock_irq(&stackbd.lock);
  bio_list_add(&stackbd.bio_list, bio);
  wake_up(&req_event);
  spin_unlock_irq(&stackbd.lock);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
  return 0;
#else
  return;
#endif
abort:
  printk("diskbox[%s]: <%p> Abort request\n\n",__func__, bio);
  bio_io_error(bio);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
  return 0;
#endif
}


static struct block_device *stackbd_bdev_open(char dev_path[])
{
  /* Open underlying device */
  struct block_device *bdev_raw = LOOKUP_BDEV(dev_path);
  if (IS_ERR(bdev_raw))
  {
    printk("diskbox[%s]: error opening raw device <%lu>\n",__func__, PTR_ERR(bdev_raw));
    return NULL;
  }
  if (!bdget(bdev_raw->bd_dev))
  {
    printk("diskbox[%s]: error bdget()\n",__func__);
    return NULL;
  }
  if (blkdev_get(bdev_raw, STACKBD_BDEV_MODE, &stackbd))
  {
    printk("diskbox[%s]: error blkdev_get()\n",__func__);
    bdput(bdev_raw);
    return NULL;
  }
  return bdev_raw;
}

int stackbd_start(char dev_path[])
{
  unsigned max_sectors;
  unsigned int page_sec = VALET_PAGE_SIZE;

  if (!(stackbd.bdev_raw = stackbd_bdev_open(dev_path)))
    return -EFAULT;
  /* Set up our internal device */
  stackbd.capacity = get_capacity(stackbd.bdev_raw->bd_disk);
  printk("diskbox[%s]: Device real capacity: %llu\n",__func__, (long long unsigned int) stackbd.capacity);
  set_capacity(stackbd.gd, stackbd.capacity-46139392);
  sector_div(page_sec, KERNEL_SECTOR_SIZE);
  max_sectors = page_sec * RDMA_WR_BUF_LEN;
  blk_queue_max_hw_sectors(stackbd.queue, max_sectors);
  stackbd.thread = kthread_create(stackbd_threadfn, NULL,stackbd.gd->disk_name);
  if (IS_ERR(stackbd.thread))
  {
    printk("diskbox[%s]: error kthread_create <%lu>\n",__func__,PTR_ERR(stackbd.thread));
    goto error_after_bdev;
  }
  printk("diskbox[%s]: done initializing successfully\n",__func__);
  stackbd.is_active = 1;
  atomic_set(&stackbd.redirect_done, STACKBD_REDIRECT_OFF);
  wake_up_process(stackbd.thread);
  return 0;
error_after_bdev:
  blkdev_put(stackbd.bdev_raw, STACKBD_BDEV_MODE);
  bdput(stackbd.bdev_raw);
  return -EFAULT;
}

int stackbd_getgeo(struct block_device * block_device, struct hd_geometry * geo)
{
  long size;
  size = stackbd.capacity * (LOGICAL_BLOCK_SIZE / KERNEL_SECTOR_SIZE);
  geo->cylinders = (size & ~0x3f) >> 6;
  geo->heads = 4;
  geo->sectors = 16;
  geo->start = 0;
  return 0;
}

static struct block_device_operations stackbd_ops = {
  .owner           = THIS_MODULE,
  .getgeo      = stackbd_getgeo,
};

int stackbd_init(void)
{
  int err = 0;

  spin_lock_init(&stackbd.lock);
  if (!(stackbd.queue = blk_alloc_queue(GFP_KERNEL)))
  {
    printk("diskbox[%s]:stackbd: alloc_queue failed\n",__func__);
    return -EFAULT;
  }
  blk_queue_make_request(stackbd.queue, stackbd_make_request);
  blk_queue_logical_block_size(stackbd.queue, LOGICAL_BLOCK_SIZE);
  if ((major_num = register_blkdev(major_num, STACKBD_NAME)) < 0)
  {
    printk("diskbox[%s]:stackbd: unable to get major number\n",__func__);
    err=-EFAULT;
    goto error_after_alloc_queue;
  }
  if (!(stackbd.gd = alloc_disk(16))){
    goto error_after_redister_blkdev;
    err=-EFAULT;
  }
  stackbd.gd->major = major_num;
  stackbd.gd->first_minor = 0;
  stackbd.gd->fops = &stackbd_ops;
  stackbd.gd->private_data = &stackbd;
  strcpy(stackbd.gd->disk_name, STACKBD_NAME_0);
  stackbd.gd->queue = stackbd.queue;
  add_disk(stackbd.gd);
  printk("diskbox[%s]:stackbd: register done\n",__func__);
  if (stackbd_start(BACKUP_DISK) < 0){
    err= -1;
  }

  goto out;

error_after_redister_blkdev:
  unregister_blkdev(major_num, STACKBD_NAME);
error_after_alloc_queue:
  blk_cleanup_queue(stackbd.queue);
  printk("diskbox[%s]: stackbd queue cleaned up\n",__func__);
out:
  return err;
}

void stackbd_exit(void)
{
  if (stackbd.is_active)
  {
    kthread_stop(stackbd.thread);
    blkdev_put(stackbd.bdev_raw, STACKBD_BDEV_MODE);
    bdput(stackbd. bdev_raw);
  }
  del_gendisk(stackbd.gd);
  put_disk(stackbd.gd);
  unregister_blkdev(major_num, STACKBD_NAME);
  blk_cleanup_queue(stackbd.queue);
}

void cleanup_stackbd_queue(void){
  blk_cleanup_queue(stackbd.queue);
}

