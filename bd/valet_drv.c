/*
 * Valet: Efficient Orchestration of Host and Remote Shared Memory
 *        for Memory IntensiveWorkloads
 * Copyright 2020 Georgia Institute of Technology.
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
#include "gpt.h"
#include "rdmabox.h"
#include "diskbox.h"
#include <linux/smp.h>

/*
#if MAX_SECTORS > RDMA_WR_BUF_LEN
#error MAX_SECTORS cannot be larger than RDMA_WR_BUF_LEN
#endif
*/

int valet_indexes;

#define DEBUG

#define DRV_NAME    "VALET"
#define PFX     DRV_NAME ": "
#define DRV_VERSION "0.0"

MODULE_AUTHOR("Juhyun Bae");
MODULE_DESCRIPTION("Valet,Efficient Orchestration of Host and Remote Shared Memory");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION(DRV_VERSION);

#define DEBUG_LOG if (1) printk

int valet_major;
struct list_head g_valet_sessions;
struct mutex g_lock;
int submit_queues;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 16, 0)
static struct blk_mq_hw_ctx *valet_alloc_hctx(struct blk_mq_reg *reg,
    unsigned int hctx_index)
{
  int b_size = DIV_ROUND_UP(reg->nr_hw_queues, nr_online_nodes); 
  int tip = (reg->nr_hw_queues % nr_online_nodes);
  int node = 0, i, n;
  struct blk_mq_hw_ctx * hctx;
  /*
   * Split submit queues evenly wrt to the number of nodes. If uneven,
   * fill the first buckets with one extra, until the rest is filled with
   * no extra.
   */
  for (i = 0, n = 1; i < hctx_index; i++, n++) {
    if (n % b_size == 0) {
      n = 0;
      node++;

      tip--;
      if (!tip)
	b_size = reg->nr_hw_queues / nr_online_nodes;
    }
  }
  /*
   * A node might not be online, therefore map the relative node id to the
   * real node id.
   */
  for_each_online_node(n) {
    if (!node)
      break;
    node--;
  }
  pr_debug("%s: n=%d\n", __func__, n);
  hctx = kzalloc_node(sizeof(struct blk_mq_hw_ctx), GFP_KERNEL, n);

  return hctx;
}

static void valet_free_hctx(struct blk_mq_hw_ctx *hctx, unsigned int hctx_index)
{
  kfree(hctx);
}
#endif

static int valet_request(struct request *req, struct valet_queue *xq)
{
  int err=0;
  size_t index;
  struct valet_session *valet_sess = xq->valet_sess;
  index = blk_rq_pos(req) >> SECTORS_PER_PAGE_SHIFT;

  get_cpu();
  switch (rq_data_dir(req)) {
    case READ:
      err = radix_read(valet_sess,req);
      break;
    case WRITE:
      err = radix_write(valet_sess,req);
      if(err){
	  put_cpu();
          return 1;
      }
      remote_sender_fn(valet_sess);
      break;
  }
  put_cpu();

  return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
static int valet_queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data *bd)
#elif LINUX_VERSION_CODE == KERNEL_VERSION(3, 18, 0)
static int valet_queue_rq(struct blk_mq_hw_ctx *hctx, struct request *rq, bool last)
#else
static int valet_queue_rq(struct blk_mq_hw_ctx *hctx, struct request *rq)
#endif
{
  struct valet_queue *valet_q;
  int err;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)
  struct request *rq = bd->rq;
#endif

  valet_q = hctx->driver_data;
  err = valet_request(rq, valet_q);

  if (unlikely(err)) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
    rq->errors = -EIO;
    return BLK_MQ_RQ_QUEUE_ERROR;
#else
    return BLK_STS_TIMEOUT;
#endif
  }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 18, 0)
  blk_mq_start_request(rq);
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)
  return BLK_STS_OK;
#else
  return BLK_MQ_RQ_QUEUE_OK;
#endif
}

static int valet_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
    unsigned int index)
{
  struct valet_file *xdev = data;
  struct valet_queue *xq;

  xq = &xdev->queues[index];
  xq->valet_sess = xdev->valet_sess;
  xq->xdev = xdev;
  xq->queue_depth = xdev->queue_depth;
  hctx->driver_data = xq;

  return 0;
}

static struct blk_mq_ops valet_mq_ops = {
  .queue_rq       = valet_queue_rq,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
  .map_queues      = blk_mq_map_queues,  
#else
  .map_queue      = blk_mq_map_queue,  
#endif
  .init_hctx	= valet_init_hctx,
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 16, 0)
  .alloc_hctx	= valet_alloc_hctx,
  .free_hctx	= valet_free_hctx,
#endif
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 16, 0)
static struct blk_mq_reg valet_mq_reg = {
  .ops		= &valet_mq_ops,
  .cmd_size	= 0,
  .flags	= BLK_MQ_F_SHOULD_MERGE,
  .numa_node	= NUMA_NO_NODE,
  .queue_depth	= VALET_QUEUE_DEPTH,
};
#endif

int valet_create_device(struct valet_session *valet_session,
    const char *xdev_name, struct valet_file *valet_file)
{
  int retval;
  sscanf(xdev_name, "%s", valet_file->file_name);
  valet_file->index = valet_indexes++;
  valet_file->nr_queues = submit_queues;
  valet_file->queue_depth = VALET_QUEUE_DEPTH;
  valet_file->valet_sess = valet_session;
  
  retval = valet_setup_queues(valet_file);
  if (retval) {
    printk("valet[%s]: valet_setup_queues failed\n", __func__);
    goto err;
  }
  valet_file->stbuf.st_size = valet_session->capacity;

  valet_session->xdev = valet_file;
  retval = valet_register_block_device(valet_file);
  if (retval) {
    printk("valet[%s]: failed to register IS device %s ret=%d\n",__func__,
        valet_file->file_name, retval);
    
    goto err_queues;
  }

  msleep(2000);
  valet_set_device_state(valet_file, DEVICE_RUNNING);
  return 0;

err_queues:
  valet_destroy_queues(valet_file);
err:
  return retval;
}

int valet_setup_queues(struct valet_file *xdev)
{
  xdev->queues = kzalloc(submit_queues * sizeof(*xdev->queues),
      GFP_KERNEL);
  if (!xdev->queues)
    return -ENOMEM;

  return 0;
}

void valet_destroy_device(struct valet_session *valet_session,
    struct valet_file *valet_file)
{
  valet_set_device_state(valet_file, DEVICE_OFFLINE);
  if (valet_file->disk){
    valet_unregister_block_device(valet_file);  
    valet_destroy_queues(valet_file);  
  }

  spin_lock(&valet_session->devs_lock);
  list_del(&valet_file->list);
  spin_unlock(&valet_session->devs_lock);
}

void valet_destroy_session_devices(struct valet_session *valet_session)
{
  struct valet_file *xdev, *tmp;
  
  list_for_each_entry_safe(xdev, tmp, &valet_session->devs_list, list) {
    valet_destroy_device(valet_session, xdev);
  }
}

struct valet_file *valet_file_find(struct valet_session *valet_session,
    const char *xdev_name)
{
  struct valet_file *pos;
  struct valet_file *ret = NULL;

  spin_lock(&valet_session->devs_lock);
  list_for_each_entry(pos, &valet_session->devs_list, list) {
    if (!strcmp(pos->file_name, xdev_name)) {
      ret = pos;
      break;
    }
  }
  spin_unlock(&valet_session->devs_lock);

  return ret;
}

struct valet_session *valet_session_find_by_portal(struct list_head *s_data_list,
    const char *portal)
{
  struct valet_session *pos;
  struct valet_session *ret = NULL;

  mutex_lock(&g_lock);
  list_for_each_entry(pos, s_data_list, list) {
    if (!strcmp(pos->portal, portal)) {
      ret = pos;
      break;
    }
  }
  mutex_unlock(&g_lock);

  return ret;
}

static int valet_open(struct block_device *bd, fmode_t mode)
{
  return 0;
}

static void valet_release(struct gendisk *gd, fmode_t mode)
{
}

static int valet_media_changed(struct gendisk *gd)
{
  return 0;
}

static int valet_revalidate(struct gendisk *gd)
{
  return 0;
}

static int valet_ioctl(struct block_device *bd, fmode_t mode,
    unsigned cmd, unsigned long arg)
{
  return -ENOTTY;
}

static struct block_device_operations valet_ops = {
  .owner           = THIS_MODULE,
  .open 	         = valet_open,
  .release 	 = valet_release,
  .media_changed   = valet_media_changed,
  .revalidate_disk = valet_revalidate,
  .ioctl	         = valet_ioctl
};

void valet_destroy_queues(struct valet_file *xdev)
{
  kfree(xdev->queues);
}

int valet_register_block_device(struct valet_file *valet_file)
{
  u64 size = valet_file->stbuf.st_size;
  int page_size = PAGE_SIZE;
  int err = 0;

  // mempool init
  err = mempool_init();
  if (err)
    goto out;

  err = gpt_init(size);
  if (err)
    goto out;
 
#ifdef DISKBACKUP
  // register stackbd
  err = stackbd_init();
  if (err)
    goto out;
#endif

  // register valet_file
  valet_file->major = valet_major;

  // set device params 
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 16, 0)
  valet_mq_reg.nr_hw_queues = submit_queues;
  valet_file->queue = blk_mq_init_queue(&valet_mq_reg, valet_file);
#else
  valet_file->tag_set.ops = &valet_mq_ops;
  valet_file->tag_set.nr_hw_queues = submit_queues;
  valet_file->tag_set.queue_depth = VALET_QUEUE_DEPTH;
  valet_file->tag_set.numa_node = NUMA_NO_NODE;
  valet_file->tag_set.cmd_size	= 0;
  valet_file->tag_set.flags = BLK_MQ_F_SHOULD_MERGE;
  valet_file->tag_set.driver_data = valet_file;

  err = blk_mq_alloc_tag_set(&valet_file->tag_set);
  if (err)
    goto out;

  valet_file->queue = blk_mq_init_queue(&valet_file->tag_set);
#endif
  if (IS_ERR(valet_file->queue)) {
    printk("valet[%s]: Failed to allocate blk queue ret=%ld\n",
	__func__, PTR_ERR(valet_file->queue));
    
    err = PTR_ERR(valet_file->queue);
    goto blk_mq_init;
  }

  valet_file->queue->queuedata = valet_file;
  queue_flag_set_unlocked(QUEUE_FLAG_NONROT, valet_file->queue);
  queue_flag_clear_unlocked(QUEUE_FLAG_ADD_RANDOM, valet_file->queue);

  valet_file->disk = alloc_disk_node(1, NUMA_NO_NODE);
  if (!valet_file->disk) {
    printk("valet[%s]: Failed to allocate disk node\n", __func__);
    
    err = -ENOMEM;
    goto alloc_disk;
  }

  valet_file->disk->major = valet_file->major;
  valet_file->disk->first_minor = valet_file->index;
  valet_file->disk->fops = &valet_ops;
  valet_file->disk->queue = valet_file->queue;
  valet_file->disk->private_data = valet_file;
  blk_queue_logical_block_size(valet_file->queue, VALET_SECT_SIZE); //block size = 512
  blk_queue_physical_block_size(valet_file->queue, VALET_SECT_SIZE);
  sector_div(page_size, VALET_SECT_SIZE);
  blk_queue_max_hw_sectors(valet_file->queue, page_size * MAX_SECTORS);
  sector_div(size, VALET_SECT_SIZE);
  set_capacity(valet_file->disk, size);  // size is in remote file state->size, add size info into block device
  sscanf(valet_file->dev_name, "%s", valet_file->disk->disk_name);
  add_disk(valet_file->disk);

  goto out;

alloc_disk:
  blk_cleanup_queue(valet_file->queue);
blk_mq_init:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 0)
  blk_mq_free_tag_set(&valet_file->tag_set);
#endif
  return err;

error_after_redister_blkdev:
  unregister_blkdev(major_num, STACKBD_NAME); 
error_after_alloc_queue:
  cleanup_stackbd_queue();
out:
  return err;
}

void valet_unregister_block_device(struct valet_file *valet_file)
{
  del_gendisk(valet_file->disk);
  blk_cleanup_queue(valet_file->queue);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 0)
  blk_mq_free_tag_set(&valet_file->tag_set);
#endif
  put_disk(valet_file->disk);
  stackbd_exit();
}

static int __init valet_init_module(void)
{
  if (valet_create_configfs_files())
    return 1;

  printk("valet[%s]: nr_cpu_ids=%d, num_online_cpus=%d\n", __func__, nr_cpu_ids, num_online_cpus());
  submit_queues = num_online_cpus();

  valet_major = register_blkdev(0, "valet");
  if (valet_major < 0)
    return valet_major;

  mutex_init(&g_lock);
  INIT_LIST_HEAD(&g_valet_sessions);

  return 0;
}

static void __exit valet_cleanup_module(void)
{
  struct valet_session *valet_session, *tmp;

  list_for_each_entry_safe(valet_session, tmp, &g_valet_sessions, list) {
    valet_session_destroy(valet_session);
  }

  valet_destroy_configfs_files();

  unregister_blkdev(valet_major, "valet");
}

module_init(valet_init_module);
module_exit(valet_cleanup_module);
