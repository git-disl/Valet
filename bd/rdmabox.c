/*
 * Valet: Efficient Orchestration of Host and Remote Shared Memory
 *        for Memory IntensiveWorkloads
 * Copyright 2020 Georgia Institute of Technology.
 *
 * Infiniswap, remote memory paging over RDMA
 * Copyright 2017 University of Michigan, Ann Arbor
 *
 * krping: Kernel Mode RDMA Ping Module
 * Steve Wise - 8/2009
 * https://github.com/larrystevenwise/krping
 *
 * Copyright (c) 2005 Ammasso, Inc. All rights reserved.
 * Copyright (c) 2006-2009 Open Grid Computing, Inc. All rights reserved.
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

#include <linux/time.h>

#include "valet_drv.h"
#include "rdmabox.h"
#include "gpt.h"
#include "valet_mempool.h"
#include "alf_queue.h"

int isRDMAon(struct valet_session *valet_sess)
{
 if(atomic_read(&valet_sess->rdma_on) == DEV_RDMA_ON)
     return 1;
 else
     return 0;
}

void put_rdma_freepool(rdma_freepool_t *pool, void *element)
{
        unsigned long flags;

        spin_lock_irqsave(&pool->rdma_freepool_lock, flags);
        BUG_ON(pool->curr_nr >= pool->cap_nr);
        pool->elements[pool->curr_nr++] = element;
        spin_unlock_irqrestore(&pool->rdma_freepool_lock, flags);
}

void *get_rdma_freepool(rdma_freepool_t *pool)
{
        unsigned long flags;
        void * element;

        spin_lock_irqsave(&pool->rdma_freepool_lock, flags);
        if (pool->curr_nr > 0) {
            BUG_ON(pool->curr_nr <= 0);
            element = pool->elements[--pool->curr_nr];
        }else{
            element = NULL;
        }
        spin_unlock_irqrestore(&pool->rdma_freepool_lock, flags);

        return element;
}

int rdma_freepool_create(struct valet_session *valet_session, rdma_freepool_t *pool, size_t cap_nr, int buf_size, int isWrite)
{
        struct rdma_ctx *ctx;	

	printk("mempool[%s]: start mempool create cap_nr=%d sizeof header: %d \n",__func__,cap_nr, sizeof(struct remote_block_header));
        spin_lock_init(&pool->rdma_freepool_lock);
        pool->init_done = 0;

        pool->elements = kzalloc(cap_nr * sizeof(void *),GFP_KERNEL);
        if (!pool->elements) {
                printk("mempool[%s]: Fail to create pool elements  \n",__func__);
                kfree(pool);
                return 1;
        }
        pool->cap_nr = cap_nr;
        pool->curr_nr = 0;

        while (pool->curr_nr < pool->cap_nr) {
           ctx = (struct rdma_ctx *)kzalloc(sizeof(struct rdma_ctx), GFP_KERNEL);
           atomic_set(&ctx->in_flight, CTX_IDLE);
           ctx->rb_index = -1;
           ctx->req = NULL;
           ctx->sending_item = NULL;
           if(isWrite){
               ctx->header_buf = kzalloc(sizeof(struct remote_block_header), GFP_KERNEL);
               if (!ctx->header_buf) {
                   printk("rdmabox[%s]: header_buf malloc failed\n",__func__);
                   kfree(ctx->header_buf);
                  return 1;
              }
           }
           ctx->rdma_buf = kzalloc(buf_size, GFP_KERNEL);
           if (!ctx->rdma_buf) {
               printk("rdmabox[%s]: rdma_buf malloc failed\n",__func__);
               kfree(ctx->rdma_buf);
	       return 1;
           }
          put_rdma_freepool(pool,ctx);

       }//while

  return 0;	
}

void rdma_freepool_post_init(struct valet_session *valet_session, struct kernel_cb *cb, rdma_freepool_t *pool, int buf_size, int isWrite)
{
        int i;
	struct rdma_ctx *ctx;

        for(i=0; i<pool->cap_nr; i++){

           ctx = pool->elements[i];

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
           ctx->rdma_dma_addr = dma_map_single(&valet_session->pd->device->dev,
                                  ctx->rdma_buf, buf_size,
                                  DMA_BIDIRECTIONAL);
           if(isWrite){
           ctx->header_dma_addr = dma_map_single(&valet_session->pd->device->dev,
                                  ctx->header_buf, sizeof(struct remote_block_header),
                                  DMA_BIDIRECTIONAL);
          }
#else
           ctx->rdma_dma_addr = dma_map_single(valet_session->pd->device->dma_device, 
	                          ctx->rdma_buf, buf_size, 
	                          DMA_BIDIRECTIONAL);
           if(isWrite){
           ctx->header_dma_addr = dma_map_single(valet_session->pd->device->dma_device, 
                                  ctx->header_buf, sizeof(struct remote_block_header), 
                                  DMA_BIDIRECTIONAL);
          }
#endif
           pci_unmap_addr_set(ctx, rdma_mapping, ctx->rdma_dma_addr);	
           if(isWrite){
           pci_unmap_addr_set(ctx, header_mapping, ctx->header_dma_addr);      
          }

    	  if(isWrite){
           ctx->rdma_sgl_wr[1].lkey = cb->qp->device->local_dma_lkey;
           ctx->rdma_sgl_wr[0].lkey = cb->qp->device->local_dma_lkey;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
           ctx->rdma_sq_wr[1].wr.send_flags = 0;
           ctx->rdma_sq_wr[1].wr.sg_list = &ctx->rdma_sgl_wr[1];
           ctx->rdma_sq_wr[1].wr.sg_list->length = sizeof(struct timeval);
           ctx->rdma_sq_wr[1].wr.num_sge = 1;
           ctx->rdma_sq_wr[1].wr.wr_id = NULL;
           ctx->rdma_sq_wr[1].wr.next = NULL;
           ctx->rdma_sq_wr[1].wr.opcode = IB_WR_RDMA_WRITE;

           ctx->rdma_sq_wr[0].wr.opcode = IB_WR_RDMA_WRITE;
           ctx->rdma_sq_wr[0].wr.send_flags = IB_SEND_SIGNALED;
           ctx->rdma_sq_wr[0].wr.sg_list = &ctx->rdma_sgl_wr[0];
           ctx->rdma_sq_wr[0].wr.num_sge = 1;
           ctx->rdma_sq_wr[0].wr.wr_id = uint64_from_ptr(ctx);
           ctx->rdma_sq_wr[0].wr.next = &ctx->rdma_sq_wr[1].wr;
#else
           ctx->rdma_sq_wr[1].send_flags = 0;
           ctx->rdma_sq_wr[1].sg_list = &ctx->rdma_sgl_wr[1];
           ctx->rdma_sq_wr[1].sg_list->length = sizeof(struct timeval);
           ctx->rdma_sq_wr[1].num_sge = 1;
           ctx->rdma_sq_wr[1].wr_id = NULL;
           ctx->rdma_sq_wr[1].next = NULL;
           ctx->rdma_sq_wr[1].opcode = IB_WR_RDMA_WRITE;

           ctx->rdma_sq_wr[0].opcode = IB_WR_RDMA_WRITE;
           ctx->rdma_sq_wr[0].send_flags = IB_SEND_SIGNALED;
           ctx->rdma_sq_wr[0].sg_list = &ctx->rdma_sgl_wr[0];
           ctx->rdma_sq_wr[0].num_sge = 1;
           ctx->rdma_sq_wr[0].wr_id = uint64_from_ptr(ctx);
           ctx->rdma_sq_wr[0].next = &ctx->rdma_sq_wr[1];
#endif
          }else{//read
           ctx->rdma_sgl_rd.addr = ctx->rdma_dma_addr;
           ctx->rdma_sgl_rd.lkey = cb->qp->device->local_dma_lkey;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
           ctx->rdma_sq_rd.wr.send_flags = IB_SEND_SIGNALED;
           ctx->rdma_sq_rd.wr.sg_list = &ctx->rdma_sgl_rd;
           ctx->rdma_sq_rd.wr.sg_list->length = VALET_PAGE_SIZE;
           ctx->rdma_sq_rd.wr.num_sge = 1;
           ctx->rdma_sq_rd.wr.wr_id = uint64_from_ptr(ctx);
	   ctx->rdma_sq_rd.wr.opcode = IB_WR_RDMA_READ;
#else
           ctx->rdma_sq_rd.opcode = IB_WR_RDMA_READ;
           ctx->rdma_sq_rd.send_flags = IB_SEND_SIGNALED;
           ctx->rdma_sq_rd.sg_list = &ctx->rdma_sgl_rd;
           ctx->rdma_sq_rd.sg_list->length = VALET_PAGE_SIZE;
           ctx->rdma_sq_rd.num_sge = 1;
           ctx->rdma_sq_rd.wr_id = uint64_from_ptr(ctx);
#endif

          }//Read
       }//while

    pool->init_done = 1;   
}

static int post_send(struct kernel_cb *cb)
{
  int ret = 0;
  struct ib_send_wr * bad_wr;

  ret = ib_post_send(cb->qp, &cb->sq_wr, &bad_wr);
  if (ret) {
    printk("rdmabox[%s]: post_send error %d\n",__func__, ret);
    
    return ret;
  }

  return 0;	
}

static void valet_migration_block_init(struct kernel_cb *cb)
{
  int i;
  struct valet_session *valet_session = cb->valet_sess;

  int replica_index = cb->recv_buf.replica_index;
  cb->rbh.client_swapspace_index = cb->recv_buf.size_gb; // client rb index
  cb->rbh.dest_cb_index = cb->recv_buf.cb_index;
  cb->rbh.dest_rb_index = cb->recv_buf.rb_index;

  for (i=0; i<MAX_MR_SIZE_GB; i++){
    if (cb->recv_buf.rkey[i]){
      cb->rbh.mig_rb_idx[i] = 'm'; // need to migrate
      cb->rbh.remote_block[i]->remote_rkey = ntohl(cb->recv_buf.rkey[i]);
      cb->rbh.remote_block[i]->remote_addr = ntohll(cb->recv_buf.buf[i]);
      cb->rbh.remote_block[i]->bitmap_g = (int *)kzalloc(sizeof(int) * BITMAP_INT_SIZE, GFP_KERNEL);
      cb->rbh.remote_block[i]->replica_index = replica_index;
      valet_bitmap_init(cb->rbh.remote_block[i]->bitmap_g);
      cb->rbh.rb_idx_map[i] = cb->rbh.client_swapspace_index;

      cb->rbh.block_size_gb += 1;
      atomic_set(cb->rbh.remote_mapped + i, RB_MAPPED);
      cb->rbh.c_state = RBH_MIGRATION_READY;
     
      printk("rdmabox[%s]: Received MIGRATION dest info. rb[%d] on server[%d] is new destination. check:%d \n", __func__,i,cb->cb_index,cb->recv_buf.size_gb);
      printk("rdmabox[%s]: rkey:0x%x addr:0x%x \n",__func__,cb->rbh.remote_block[i]->remote_rkey,cb->rbh.remote_block[i]->remote_addr);
    }else{
      cb->rbh.mig_rb_idx[i] = 0;
    }
  }
}

void valet_remote_block_init(struct kernel_cb *cb)
{
  int i = 0;
  struct remote_block *rb;
  int select_idx = cb->recv_buf.size_gb;
  int replica_index = cb->recv_buf.replica_index;
  struct valet_session *valet_session = cb->valet_sess;

  for (i = 0; i < MAX_MR_SIZE_GB; i++) {
    if (cb->recv_buf.rkey[i]) {
      cb->rbh.remote_block[i]->remote_rkey = ntohl(cb->recv_buf.rkey[i]);
      cb->rbh.remote_block[i]->remote_addr = ntohll(cb->recv_buf.buf[i]);
      cb->rbh.remote_block[i]->bitmap_g = (int *)kzalloc(sizeof(int) * BITMAP_INT_SIZE, GFP_KERNEL);
      cb->rbh.remote_block[i]->replica_index = replica_index;
      valet_bitmap_init(cb->rbh.remote_block[i]->bitmap_g);
      valet_session->mapping_swapspace_to_rb[replica_index][select_idx] = i;
      cb->rbh.rb_idx_map[i] = select_idx;

      cb->rbh.block_size_gb += 1;
      cb->rbh.c_state = RBH_READY;
      atomic_set(cb->rbh.remote_mapped + i, RB_MAPPED);
      atomic_set(valet_session->cb_index_map[replica_index] + (select_idx), cb->cb_index);

      rb = cb->rbh.remote_block[i];
      rb->rb_state = ACTIVE; 

      printk("rdmabox[%s]: replica_index:%d (rb[%d] on node[%d]) is mapped to swapspace[%d]\n",__func__,replica_index,i,cb->cb_index,select_idx);
      break;
    }
  }
}

inline int valet_set_device_state(struct valet_file *xdev,
    enum valet_dev_state state)
{
  int ret = 0;

  spin_lock(&xdev->state_lock);
  switch (state) {
    case DEVICE_OPENNING:
      if (xdev->state == DEVICE_OFFLINE ||
	  xdev->state == DEVICE_RUNNING) {
	ret = -EINVAL;
	goto out;
      }
      xdev->state = state;
      break;
    case DEVICE_RUNNING:
      xdev->state = state;
      break;
    case DEVICE_OFFLINE:
      xdev->state = state;
      break;
    default:
      printk("rdmabox[%s]:Unknown device state %d\n",__func__, state);
      
      ret = -EINVAL;
  }
out:
  spin_unlock(&xdev->state_lock);
  return ret;
}

int rdma_read(struct valet_session *valet_sess, struct kernel_cb *cb, int cb_index, int rb_index, struct remote_block *rb, unsigned long offset, struct request *req)
{
  int ret;
  int err;
  struct ib_send_wr *bad_wr;
  struct rdma_ctx *ctx;
  
retry:
  ctx = (struct rdma_ctx *)get_rdma_freepool(&valet_sess->rdma_read_freepool);
  while (!ctx){
    printk("rdmabox[%s]: try to get read ctx \n",__func__);
    goto retry;
  }

  get_cpu();
  ctx->rb_index = rb_index; //rb_index in cb
  ctx->cb = cb;
  ctx->req = req;
  atomic_set(&ctx->in_flight, CTX_R_IN_FLIGHT);
  
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
  ctx->rdma_sq_rd.rkey = rb->remote_rkey;
  ctx->rdma_sq_rd.remote_addr = rb->remote_addr + sizeof(struct remote_block_header) + offset;
#else
  ctx->rdma_sq_rd.wr.rdma.rkey = rb->remote_rkey;
  ctx->rdma_sq_rd.wr.rdma.remote_addr = rb->remote_addr + sizeof(struct remote_block_header) + offset;
#endif	
  ret = ib_post_send(cb->qp, &ctx->rdma_sq_rd, &bad_wr);
  if (ret) {
    printk("rdmabox[%s]:client post read error ret=%d\n",__func__, ret);
    put_cpu();
    return ret;
  }
  put_cpu();

  return 0;
}

struct kernel_cb* get_cb(struct valet_session *valet_session, int swapspace_index, struct kernel_cb **replica_cb_list)
{
    int i;
    int ret;
    int cb_index;

    spin_lock(&valet_session->cb_lock);
    cb_index = atomic_read(valet_session->cb_index_map[0] + swapspace_index);
    if (cb_index == NO_CB_MAPPED){
        ret = valet_remote_block_map(valet_session, swapspace_index, replica_cb_list);
        if(ret != NUM_REPLICA){
            spin_unlock(&valet_session->cb_lock);
	    return NULL;
        }
     }else{
        replica_cb_list[0] = valet_session->cb_list[cb_index];
        for(i=1; i<NUM_REPLICA; i++){
            cb_index = atomic_read(valet_session->cb_index_map[i] + swapspace_index);
            replica_cb_list[i] = valet_session->cb_list[cb_index];
	}
     }
     spin_unlock(&valet_session->cb_lock);
out:
     return replica_cb_list[0];
}

int __rdma_write(struct valet_session *valet_sess, struct kernel_cb *cb,  int swapspace_index, unsigned long offset, int replica_index, struct rdma_ctx *ctx)
{
   int ret;
   struct ib_send_wr *bad_wr;	
   int rb_index;
   struct remote_block *rb;

   rb_index = valet_sess->mapping_swapspace_to_rb[replica_index][swapspace_index];
   rb = cb->rbh.remote_block[rb_index];
   
   ctx->offset = offset;
   ctx->cb = cb;
   ctx->rb = rb;
   ctx->rb_index = rb_index;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
   ctx->rdma_sq_wr[1].rkey = rb->remote_rkey;
   ctx->rdma_sq_wr[1].remote_addr = rb->remote_addr;
   ctx->rdma_sq_wr[0].rkey = rb->remote_rkey;
   ctx->rdma_sq_wr[0].remote_addr = rb->remote_addr + sizeof(struct remote_block_header) + offset;
#else
   ctx->rdma_sq_wr[1].wr.rdma.rkey = rb->remote_rkey;
   ctx->rdma_sq_wr[1].wr.rdma.remote_addr = rb->remote_addr;
   ctx->rdma_sq_wr[0].wr.rdma.rkey = rb->remote_rkey;
   ctx->rdma_sq_wr[0].wr.rdma.remote_addr = rb->remote_addr + sizeof(struct remote_block_header) + offset;
#endif
   ret = ib_post_send(cb->qp, &ctx->rdma_sq_wr[0], &bad_wr);
   if (ret) {
          printk("rdmabox[%s]: post write error. ret=%d, index=%zu\n",__func__, ret, ctx->sending_item->start_index);
          return ret;
   }
  return 0;
}

int rdma_write(struct valet_session *valet_session, int swapspace_index, unsigned long offset, struct local_page_list *tmp)
{
  int i,j;
  int ret;
  struct rdma_ctx *ctx[NUM_REPLICA];
  unsigned char *cmem;
  struct timeval cur;
  struct kernel_cb *cb;
  struct remote_block *rb;
  int rb_index;
  struct kernel_cb *replica_cb_list[NUM_REPLICA];

  get_cpu();
  //find primary cb
  cb = get_cb(valet_session, swapspace_index, replica_cb_list);
  if(!cb){
      printk("brd[%s]: cb is null \n", __func__);
      return 1;
  }

  //find primary rb 
  rb_index = valet_session->mapping_swapspace_to_rb[0][swapspace_index];
  rb = cb->rbh.remote_block[rb_index];

  // check if READONLY
  if(rb->rb_state >= READONLY){
         printk("rdmabox[%s]: this rb is READONLY. MIGRATION on progress\n",__func__);
try_again:
         ret = put_sending_index(tmp);
         if(ret){
             printk("brd[%s]: Fail to put sending index. retry \n", __func__);
             goto try_again;
         }
         put_cpu();
         return 0;
  }

  do_gettimeofday(&cur);

  atomic_set(&tmp->ref_count, 0);

  for(j=0; j<NUM_REPLICA; j++){
retry:
      ctx[j] = (struct rdma_ctx *)get_rdma_freepool(&valet_session->rdma_write_freepool);
      while (!ctx[j]){
              printk("rdmabox[%s]: try to get write ctx[%d] \n",__func__,j);
              goto retry;
      }
      atomic_set(&ctx[j]->in_flight, CTX_W_IN_FLIGHT);
      ctx[j]->replica_index = j;

      if(j==0){
          memcpy(ctx[j]->header_buf, &cur, sizeof(struct timeval));
          for (i=0; i < tmp->len;i++){
              struct tree_entry *entry;
              entry = tmp->batch_list[i];

              if(unlikely(!entry->page)){
                  printk("rdmabox[%s]: entry->page null i=%d, len=%d \n",__func__,i,tmp->len);
                  continue;
              }
              cmem = kmap_atomic(entry->page);
              memcpy(ctx[j]->rdma_buf + (i*VALET_PAGE_SIZE), cmem, VALET_PAGE_SIZE);
              kunmap_atomic(cmem);
          }
           ctx[j]->rdma_sgl_wr[1].addr = ctx[j]->header_dma_addr;
           ctx[j]->rdma_sgl_wr[0].addr = ctx[j]->rdma_dma_addr;
      }else{
           ctx[j]->rdma_sgl_wr[1].addr = ctx[0]->header_dma_addr;
           ctx[j]->rdma_sgl_wr[0].addr = ctx[0]->rdma_dma_addr;
           ctx[j]->prime_ctx = ctx[0];
      }
      ctx[j]->len = tmp->len * VALET_PAGE_SIZE;
      ctx[j]->sending_item = tmp;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
      ctx[j]->rdma_sq_wr[0].wr.sg_list->length = tmp->len * VALET_PAGE_SIZE;
#else
      ctx[j]->rdma_sq_wr[0].sg_list->length = tmp->len * VALET_PAGE_SIZE;
#endif
  }//replica forloop

#ifdef DISKBACKUP
  disk_write(ctx[0], tmp->cloned_bio, tmp->len*VALET_PAGE_SIZE, tmp->start_index);
#endif

  // send out
  ret = __rdma_write(valet_session, replica_cb_list[0], swapspace_index, offset, 0, ctx[0]);
  if (ret){ 
      put_cpu();
      return ret;
  }
  for(j=1; j<NUM_REPLICA; j++){
     ret =  __rdma_write(valet_session, replica_cb_list[j], swapspace_index, offset, j, ctx[j]);
     if (ret){
        put_cpu();
        return ret;
     }
  }

   put_cpu();
   //update write ops
   //spin_lock_irqsave(&cb->wr_ops_lock, flags);
   //valet_session->wr_ops[swapspace_index][valet_session->pos_wr_hist] += tmp->len;
   //valet_session->wr_ops[swapspace_index][0] += 1;
   //spin_unlock_irqrestore(&cb->wr_ops_lock, flags);

  return 0;
}
/*
static int valet_check_activity(struct valet_session *valet_sess, int rb_index)
{
  int i,j;
  unsigned long wr_sum=0;
  unsigned long wr_sum_prev=0;
  unsigned long rd_sum=0;
  unsigned long rd_sum_prev=0;

  // current window reset
  wr_sum_prev = valet_sess->wr_ops[rb_index][0];
  rd_sum_prev = valet_sess->rd_ops[rb_index][0];

  msleep(1000);

  wr_sum = valet_sess->wr_ops[rb_index][0];
  rd_sum = valet_sess->rd_ops[rb_index][0];

  printk("rdmabox[%s]: swapspace[%d] RD activity:%lu , WR activity:%lu \n",__func__,rb_index, (rd_sum-rd_sum_prev), (wr_sum-wr_sum_prev));
  return rd_sum ;
}
*/
static int reset_migration_info(struct kernel_cb *old_cb, struct kernel_cb *new_cb, int replica_index, int client_evict_rb_index)
{
  int i;
  struct valet_session *valet_sess = old_cb->valet_sess;

  printk("rdmabox[%s]: now reset migration information \n",__func__);

  for (i = 0; i < MAX_MR_SIZE_GB; i++) {
    if (old_cb->rbh.mig_rb_idx[i] == 'm'){
      old_cb->rbh.remote_block[i]->remote_rkey = 0;
      old_cb->rbh.remote_block[i]->remote_addr = 0;
      valet_bitmap_init(old_cb->rbh.remote_block[i]->bitmap_g);
      old_cb->rbh.rb_idx_map[i] = -1;

      old_cb->rbh.block_size_gb -= 1;
      old_cb->rbh.c_state = RBH_IDLE;
      atomic_set(old_cb->rbh.remote_mapped + i, RB_UNMAPPED);

      old_cb->rbh.mig_rb_idx[i] = 0;
      atomic_set(valet_sess->cb_index_map[replica_index] + client_evict_rb_index, NO_CB_MAPPED);

      printk("rdmabox[%s]: (rb[%d] on server[%d]) is cleaned now.\n",__func__,i,old_cb->cb_index);
    }
    if(new_cb)
        new_cb->rbh.mig_rb_idx[i] = 0;
  }

  return 0;
}

static void set_rb_state(struct kernel_cb *cb, int select_idx, enum remote_block_state state)
{
   int rb_index;
   struct remote_block *rb;
   struct valet_session *valet_sess = cb->valet_sess;

   rb_index = valet_sess->mapping_swapspace_to_rb[0][select_idx];
   rb = cb->rbh.remote_block[rb_index];
   rb->rb_state = state; 
   printk("rdmabox[%s]: swapspace:%d, rb_index:%d, rb->rb_state:%d  \n",__func__,select_idx,rb_index,rb->rb_state);
   
}

int request_read(struct valet_session *valet_sess, struct request *req)
{
  int i;
  int retval = 0;
  int rst = 1;
  int swapspace_index;
  int replica_index;
  unsigned long rb_offset;	
  struct kernel_cb *cb;
  int cb_index;
  int rb_index;
  struct remote_block *rb;
  int bitmap_i;
  size_t start = blk_rq_pos(req) << SECTOR_SHIFT;

  if(!isRDMAon(valet_sess))
      return 1;

  swapspace_index = start >> ONE_GB_SHIFT;
  rb_offset = start & ONE_GB_MASK;	
  bitmap_i = (int)(rb_offset / VALET_PAGE_SIZE);

  for(i=0; i<NUM_REPLICA; i++){
      replica_index = i;
      cb_index = atomic_read(valet_sess->cb_index_map[i] + swapspace_index);
      if(cb_index == NO_CB_MAPPED){
	  rst = 1;
          continue;
      }
      cb = valet_sess->cb_list[cb_index];

      rb_index = valet_sess->mapping_swapspace_to_rb[replica_index][swapspace_index];
      rb = cb->rbh.remote_block[rb_index];

      // check if READONLY or NONE_ALLOWED
      if(rb->rb_state == READONLY || rb->rb_state == NONE_ALLOWED){
          printk("rdmabox[%s]: this rb is READONLY or NONE_ALLOWED.\n",__func__);
          continue;
      }

      rst = valet_bitmap_test(rb->bitmap_g, bitmap_i);
      if (rst){ //remote recorded
          retval = rdma_read(valet_sess, cb, cb_index, rb_index, rb, rb_offset, req); 
          if (unlikely(retval)) {
	    printk("rdmabox[%s]: failed to read from remote\n",__func__);
	    rst = 1;
	    continue;
          }
          //update read ops
          //spin_lock_irqsave(&cb->wr_ops_lock, flags);
          //valet_sess->rd_ops[swapspace_index][valet_sess->pos_rd_hist] += 1;
          //valet_sess->rd_ops[swapspace_index][0] += 1;
          //spin_unlock_irqrestore(&cb->wr_ops_lock, flags);
	  return 0;
      }else{
	  rst = 1;
      }

  }//for

  return rst;
}

int request_write(struct valet_session *valet_sess, struct local_page_list *tmp)
{
  int retval = 0;
  int i;
  int swapspace_index, sec_swapspace_index;
  int boundary_index, sec_boundary_index;
  unsigned long rb_offset, sec_rb_offset;	
  struct local_page_list *sec_tmp;
  size_t start = tmp->start_index << (SECTORS_PER_PAGE_SHIFT + SECTOR_SHIFT); // PAGE_SHIFT - SECTOR_SHIFT + SECTOR_SHIFT
  
  swapspace_index = start >> ONE_GB_SHIFT;
  boundary_index = (start + (tmp->len*VALET_PAGE_SIZE) - VALET_PAGE_SIZE) >> ONE_GB_SHIFT;
  rb_offset = start & ONE_GB_MASK;	

  // check border remote block
  if (swapspace_index != boundary_index) {
repeat:
       sec_tmp = get_free_item();
       if(unlikely(!sec_tmp)){
          printk("brd[%s]: Fail to allocate list item. retry \n", __func__);
          goto repeat;
       }

       // find out the border
       size_t border_index=0;
       for(i=0; i<tmp->len ;i++){
           sec_boundary_index = (start + (border_index*VALET_PAGE_SIZE)) >> ONE_GB_SHIFT;
           if(swapspace_index != sec_boundary_index)
	       break;

	   border_index++;
       }
       // seperate tmp entry
       int j=0;
       for(i=border_index; i<tmp->len ;i++){
	  sec_tmp->batch_list[j] = tmp->batch_list[i];
          j++;
	}
       sec_tmp->start_index= tmp->start_index + border_index;
       sec_tmp->len= tmp->len - border_index;
       tmp->len= border_index;

       size_t sec_start = sec_tmp->start_index << (SECTORS_PER_PAGE_SHIFT + SECTOR_SHIFT);
       sec_swapspace_index = sec_start >> ONE_GB_SHIFT;
       sec_rb_offset = sec_start & ONE_GB_MASK;	

#ifdef DISKBACKUP
       struct bio *cloned_bio;
       cloned_bio = create_bio_copy(tmp->cloned_bio);
       if (unlikely(!cloned_bio)) {
           printk("brd[%s]: fail to clone bio \n",__func__);
           return 1;
       }
       sec_tmp->cloned_bio = cloned_bio;
#endif

      // send second half of entry
      retval = rdma_write(valet_sess, sec_swapspace_index, sec_rb_offset, sec_tmp); 
      if (unlikely(retval)) {
          printk("rdmabox[%s]: failed to write on remote\n", __func__);
          return 1;
      }
   }

   retval = rdma_write(valet_sess, swapspace_index, rb_offset, tmp); 
   if (unlikely(retval)) {
      printk("rdmabox[%s]: failed to write on remote\n", __func__);
      return 1;
   }
   return 0;
}

static int valet_disconnect_handler(struct kernel_cb *cb)
{
  int pool_index = cb->cb_index;
  int i, j=0;
  struct valet_session *valet_sess = cb->valet_sess;
  int *idx_map = cb->rbh.rb_idx_map;
  int sess_rb_index;
  int err = 0;
  int evict_list[SWAPSPACE_SIZE_G];
  struct request *req;
  struct rdma_ctx *ctx;
  rdma_freepool_t *ctx_pool;
  struct kernel_cb *replica_cb_list[NUM_REPLICA];

  for (i=0; i<SWAPSPACE_SIZE_G;i++){
    evict_list[i] = -1;
  }

  if (valet_sess->cb_state_list[cb->cb_index] == CB_CONNECTED){
    printk("rdmabox[%s]: connected_cb [%d] is disconnected\n", __func__, cb->cb_index);

    valet_sess->cb_state_list[cb->cb_index] = CB_FAIL;
    return cb->cb_index;
  }

  valet_sess->cb_state_list[cb->cb_index] = CB_FAIL;
  atomic_set(&cb->valet_sess->rdma_on, DEV_RDMA_OFF);

  for (i = 0; i < MAX_MR_SIZE_GB; i++) {
    sess_rb_index = idx_map[i];
    if (sess_rb_index != -1) {
      evict_list[sess_rb_index] = 1;
      valet_bitmap_init(cb->rbh.remote_block[i]->bitmap_g);
      atomic_set(cb->rbh.remote_mapped + i, RB_UNMAPPED);
      atomic_set(valet_sess->cb_index_map + (sess_rb_index), NO_CB_MAPPED);
      printk("rdmabox[%s]: unmap rb %d\n", __func__, sess_rb_index);
    }
  }

  printk("rdmabox[%s]: unmap %d GB in cb%d \n", __func__, cb->rbh.block_size_gb, pool_index);

  cb->rbh.block_size_gb = 0;

  msleep(10);

    ctx_pool = &valet_sess->rdma_write_freepool;
    for (j=0; j < ctx_pool->cap_nr; j++){
      ctx = (struct rdma_ctx *)ctx_pool->elements[i];
      switch (atomic_read(&ctx->in_flight)){
        case CTX_W_IN_FLIGHT:
          atomic_set(&ctx->in_flight, CTX_IDLE);
          if (ctx->req == NULL){
            break;
          }
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 18, 0)
          blk_mq_end_request(ctx->req, 0);
#else
          blk_mq_end_io(ctx->req, 0);
#endif
          break;
        default:
          ;
      }
    }
    ctx_pool = &valet_sess->rdma_read_freepool;
    for (j=0; j < ctx_pool->cap_nr; j++){
      ctx = (struct rdma_ctx *)ctx_pool->elements[i];
      switch (atomic_read(&ctx->in_flight)){
        case CTX_R_IN_FLIGHT:
          req = ctx->req;
          atomic_set(&ctx->in_flight, CTX_IDLE);
          disk_write_bio_clone(req);
          put_rdma_freepool(ctx_pool,ctx);
          break;
        default:
          ;
      }
    }
  printk("rdmabox[%s]: finish handling in-flight request\n", __func__);

  atomic_set(&cb->valet_sess->rdma_on, DEV_RDMA_ON);
  for (i=0; i<SWAPSPACE_SIZE_G; i++){
    if (evict_list[i] == 1){
      valet_remote_block_map(valet_sess, i, replica_cb_list);
    }
  }

  return err;
}

static int valet_cma_event_handler(struct rdma_cm_id *cma_id,
    struct rdma_cm_event *event)
{
  int ret;
  struct kernel_cb *cb = cma_id->context;

  switch (event->event) {
    case RDMA_CM_EVENT_ADDR_RESOLVED:
      cb->state = ADDR_RESOLVED;
      ret = rdma_resolve_route(cma_id, 2000);
      if (ret) {
	printk("rdmabox[%s]: rdma_resolve_route error %d\n",__func__,ret);
	wake_up_interruptible(&cb->sem);
      }
      break;
    case RDMA_CM_EVENT_ROUTE_RESOLVED:
      cb->state = ROUTE_RESOLVED;
      wake_up_interruptible(&cb->sem);
      break;
    case RDMA_CM_EVENT_CONNECT_REQUEST:
      cb->state = CONNECT_REQUEST;
      cb->child_cm_id = cma_id;
      wake_up_interruptible(&cb->sem);
      break;
    case RDMA_CM_EVENT_ESTABLISHED:
      cb->state = CONNECTED;
      wake_up_interruptible(&cb->sem);
      if (atomic_dec_and_test(&cb->valet_sess->conns_count)) {
	complete(&cb->valet_sess->conns_wait);
      }
      break;

    case RDMA_CM_EVENT_ADDR_ERROR:
    case RDMA_CM_EVENT_ROUTE_ERROR:
    case RDMA_CM_EVENT_CONNECT_ERROR:
    case RDMA_CM_EVENT_UNREACHABLE:
    case RDMA_CM_EVENT_REJECTED:
      printk("rdmabox[%s]: cma event %d, error %d\n",__func__, event->event,
	  event->status);
      cb->state = ERROR;
      wake_up_interruptible(&cb->sem);
      break;
    case RDMA_CM_EVENT_DISCONNECTED:
      printk("rdmabox[%s]: DISCONNECT EVENT...\n",__func__);
      cb->state = CM_DISCONNECT;
      valet_disconnect_handler(cb);
      break;
    case RDMA_CM_EVENT_DEVICE_REMOVAL:
      printk("rdmabox[%s]: cma detected device removal!!!!\n",__func__);
      break;
    default:
      printk("rdmabox[%s]: oof bad type!\n",__func__);
      wake_up_interruptible(&cb->sem);
      break;
  }
  return 0;
}

static int valet_wait_in_flight_requests(struct valet_session *valet_session, uint32_t rkey, int writeonlycheck)
{
  int i;
  int wr_active=0;
  int rd_active=0;
  struct rdma_ctx *ctx;
  rdma_freepool_t *ctx_pool;

  while (1) {
       // write pool
    ctx_pool = &valet_session->rdma_write_freepool;
    for (i=0; i < ctx_pool->cap_nr; i++){
           ctx = (struct rdma_ctx *)ctx_pool->elements[i];
           if(!ctx){
		continue;
           }
           if(!ctx->rb){
		continue;
           }
           if(ctx->rb->remote_rkey != rkey){
		continue;
           }
           if(atomic_read(&ctx->in_flight) == CTX_W_IN_FLIGHT){
               printk("rdmabox[%s]: %d in write rb_index %d\n", __func__, i, ctx->rb_index);
               wr_active += 1;
            }
    }//for

    if(!writeonlycheck){
       // write pool
       ctx_pool = &valet_session->rdma_read_freepool;
       for (i=0; i < ctx_pool->cap_nr; i++){
           ctx = (struct rdma_ctx *)ctx_pool->elements[i];
           if(!ctx){
		continue;
           }
           if(!ctx->rb){
		continue;
           }
           if(ctx->rb->remote_rkey != rkey){
		continue;
           }
           if(atomic_read(&ctx->in_flight) == CTX_R_IN_FLIGHT){
               printk("rdmabox[%s]: %d in read rb_index %d\n", __func__, i, ctx->rb_index);
               rd_active += 1;
            }
        }//for
    }

    if (wr_active == 0 && rd_active == 0){
        printk("rdmabox[%s]: all in_flight done wr:%d rd:%d \n", __func__,wr_active, rd_active);
        return 0;
    }else{
        printk("rdmabox[%s]: still waiting.. wr:%d rd:%d \n", __func__,wr_active, rd_active);
        wr_active=0;
        rd_active=0;
    }
  }//while

  return 1; 
}

int remote_sender_fn(struct valet_session *valet_sess)
{
  struct local_page_list *tmp_first=NULL;
  int err;
  int i;
  int must_go = 0;

  while(true){
        struct local_page_list *tmp;
        err = get_sending_index(&tmp);
        if(err){
            if(tmp_first){
                tmp = NULL;
                goto send;
            }
            return 0;
        }

        if(!tmp_first)
        {
            tmp_first=tmp;

            if(must_go){
                 tmp = NULL;
                 goto send;
             }else{
                 continue;
             }
        }

        // check if there is contiguous item
        // check if it fits into one item
        if( (tmp->start_index == tmp_first->start_index + tmp_first->len) &&
            (tmp_first->len + tmp->len <= RDMA_WR_BUF_LEN) )
        {
             // merge items
             int j = tmp_first->len;
             for(i=0; i < tmp->len ;i++){
                 tmp_first->batch_list[j] = tmp->batch_list[i];
                 j++;
             }
             tmp_first->len = tmp_first->len + tmp->len;

             // release unused item
             put_free_item(tmp);
             continue;
        }
send:
        // write remote
        err = request_write(valet_sess, tmp_first);
        if(err){
            printk("rdmabox[%s]: remote write fail\n", __func__);
try_again:
            err = put_sending_index(tmp_first);
            if(err){
                 printk("rdmabox[%s]: Fail to put sending index. retry \n", __func__);
                 msleep(1);
                 goto try_again;
            }
        }

        if(!tmp){
            must_go = 0;
            return 0;
        }else{
            must_go = 1;
            tmp_first = tmp;
        }

      }//while 
}

//migration
static int migration_handler(void *data)
{
  struct kernel_cb *old_cb = data;	
  struct kernel_cb *new_cb = NULL;
  int client_evict_rb_index;
  int src_cb_index;
  int src_rb_index;
  int dest_cb_index;
  int dest_rb_index;
  int replica_index;
  int i,j;
  int err = 0;
  struct valet_session *valet_sess = old_cb->valet_sess;
  int rb_index;
  unsigned long wr_sum=0;
  unsigned long wr_sum_prev=0;
  unsigned long rd_sum=0;
  unsigned long rd_sum_prev=0;

  while (old_cb->state != ERROR) {
    wait_event_interruptible(old_cb->rbh.sem, (old_cb->rbh.c_state == RBH_EVICT));	
    // client rb index
    replica_index = old_cb->rbh.replica_index;
    client_evict_rb_index = old_cb->rbh.client_swapspace_index;
    src_cb_index = old_cb->rbh.src_cb_index;
    src_rb_index = old_cb->rbh.src_rb_index;
    printk("rdmabox[%s]: received EVICT message from node %d \n", __func__,src_cb_index);

    // migration
    printk("rdmabox[%s]: Set CB_MIGRATING state to node %d \n", __func__,src_cb_index);
    valet_sess->cb_state_list[src_cb_index] = CB_MIGRATING;

    // stop write to server
    printk("rdmabox[%s]: set client swapspace[%d] READONLY \n", __func__,client_evict_rb_index);
    set_rb_state(old_cb, client_evict_rb_index, READONLY);

    // remote allocation for migration destination
    // search new destination
    
    new_cb = valet_migration_block_map(valet_sess, client_evict_rb_index, old_cb, src_cb_index, replica_index); //wait inside until RESP_MIG_MAP 
    if(!new_cb){
       printk("rdmabox[%s]: fail to find the migration destination. Send Delete. \n", __func__);
       printk("rdmabox[%s]: wait inflight read, write ops.. \n", __func__);
       valet_wait_in_flight_requests(valet_sess, old_cb->rbh.remote_block[src_rb_index]->remote_rkey, 0);
       printk("rdmabox[%s]: done inflight read, write ops.. \n", __func__);

       // now send delete rb msg to server
       for (i=0; i<MAX_MR_SIZE_GB; i++) {
            if(old_cb->rbh.mig_rb_idx[i] == 'm'){
	        old_cb->send_buf.rkey[i] = 1; 
            }else{
	        old_cb->send_buf.rkey[i] = 0; 
            }
       }
       old_cb->send_buf.type = DELETE_RB;
       post_send(old_cb);

       printk("rdmabox[%s]: wait RBH_DELETE_RB \n", __func__);
       wait_event_interruptible(old_cb->rbh.sem, (old_cb->rbh.c_state == RBH_DELETE_RB));	
       printk("rdmabox[%s]: RBH_DELETE_RB done \n", __func__);

       // clean rb info from old_cb
       reset_migration_info(old_cb, NULL, replica_index, client_evict_rb_index);

       set_rb_state(old_cb, client_evict_rb_index, INACTIVE);

       continue;
    }

    dest_cb_index = new_cb->rbh.dest_cb_index;
    dest_rb_index = new_cb->rbh.dest_rb_index;
   
    printk("rdmabox[%s]: wait inflight write ops.. \n", __func__);
    valet_wait_in_flight_requests(valet_sess, old_cb->rbh.remote_block[src_rb_index]->remote_rkey, 1);
    printk("rdmabox[%s]: done inflight write ops.. \n", __func__);

    printk("rdmabox[%s]: wait MIGRATION_DONE \n", __func__);
    wait_event_interruptible(old_cb->rbh.sem, (old_cb->rbh.c_state == RBH_MIGRATION_DONE));	
    // updated destination info in this client
    // now we have all set for new remote destination(copying to new dest is done)
    printk("rdmabox[%s]: received MIGRATION_DONE \n", __func__);
    // update mr information with new destination
    atomic_set(valet_sess->cb_index_map[0] + (client_evict_rb_index), dest_cb_index);
    valet_sess->mapping_swapspace_to_rb[0][client_evict_rb_index]=dest_rb_index;
    new_cb->rbh.c_state = RBH_READY;
    memcpy(new_cb->rbh.remote_block[dest_rb_index]->bitmap_g,old_cb->rbh.remote_block[src_rb_index]->bitmap_g,sizeof(int) * BITMAP_INT_SIZE);
    printk("rdmabox[%s]: update migration info. rb[%d] on server[%d] is destination.\n", __func__,dest_rb_index,dest_cb_index);

    // wait for in flight ops to previous evicting server
    valet_wait_in_flight_requests(valet_sess, old_cb->rbh.remote_block[src_rb_index]->remote_rkey, 0);

    // now send delete rb msg to server
    for (i=0; i<MAX_MR_SIZE_GB; i++) {
            if(old_cb->rbh.mig_rb_idx[i] == 'm'){
	        old_cb->send_buf.rkey[i] = 1; 
            }else{
	        old_cb->send_buf.rkey[i] = 0; 
            }
    }

    old_cb->send_buf.type = DELETE_RB;
    post_send(old_cb);
    wait_event_interruptible(old_cb->rbh.sem, (old_cb->rbh.c_state == RBH_DELETE_RB));	
    printk("rdmabox[%s]: send RBH_DELETE_RB done \n", __func__);

    // clean old_cb new_cb info   
    reset_migration_info(old_cb, new_cb, replica_index, client_evict_rb_index);
    valet_sess->cb_state_list[src_cb_index] = CB_MAPPED;

    set_rb_state(old_cb, client_evict_rb_index, INACTIVE);

  }//while

  return err;
}

static void evict_handler_prep(struct kernel_cb *cb) 
{
  int i;
  cb->rbh.client_swapspace_index = cb->recv_buf.size_gb; //client_rb_index
  cb->rbh.src_cb_index = cb->recv_buf.cb_index; //src_cb_index	
  cb->rbh.src_rb_index = cb->recv_buf.rb_index; //src_rb_index	
  cb->rbh.replica_index = cb->recv_buf.replica_index;

  for (i=0; i<MAX_MR_SIZE_GB; i++){
    if (cb->recv_buf.rkey[i]){
      cb->rbh.mig_rb_idx[i] = 'm'; // need to migrate
      printk("rdmabox[%s]: evict mr_info rkey:0x%x addr:0x%x \n",__func__,cb->recv_buf.rkey[i],cb->recv_buf.buf[i]);
    }else{
      cb->rbh.mig_rb_idx[i] = 0; // not related
    }
  }
}

static int recv_msg_handler(struct kernel_cb *cb, struct ib_wc *wc)
{
  struct query_message qmsg;

  if (wc->byte_len != sizeof(cb->recv_buf)) {
    printk("rdmabox[%s]: Received bogus data, size %d\n",__func__,wc->byte_len);
    return -1;
  }	
  if (cb->state < CONNECTED){
    printk("rdmabox[%s]: cb is not connected\n",__func__);	
    return -1;
  }
  switch(cb->recv_buf.type){
    case RESP_QUERY:
      memcpy(&qmsg,&cb->recv_buf,sizeof(qmsg));
      printk("rdmabox[%s]: Received RESP_QUERY free_mem : %d, sum : %lu node:%d\n", __func__,qmsg.size_gb, qmsg.sum, cb->cb_index);
      cb->rbh.target_size_gb = qmsg.size_gb;
      cb->rbh.sum = qmsg.sum;
      cb->state = RECV_QUERY_REPL;	
      break;
    case RESP_MIG_MAP:
      printk("rdmabox[%s]: Received RESP_MIG_MAP from node[%d] \n", __func__,cb->cb_index);
      cb->state = RECV_MIG_MAP;
      valet_migration_block_init(cb);
      break;
    case RESP_MR_MAP:
      printk("rdmabox[%s]: Received RESP_MR_MAP from node[%d] \n", __func__,cb->cb_index);
      cb->valet_sess->cb_state_list[cb->cb_index] = CB_MAPPED;
      cb->state = RECV_MR_MAP;
      valet_remote_block_init(cb);
      break;
    case DONE_MIGRATION:
      cb->state = RECV_MIGRATION_DONE;
      cb->rbh.c_state = RBH_MIGRATION_DONE;
      printk("rdmabox[%s]: Received DONE_MIGRATION from node[%d] \n", __func__,cb->cb_index);
      wake_up_interruptible(&cb->rbh.sem);
      break;
    case EVICT:
      cb->state = RECV_EVICT;
      evict_handler_prep(cb);
      cb->rbh.c_state = RBH_EVICT;
      printk("rdmabox[%s]: Received EVICT from node[%d] \n", __func__,cb->cb_index);
      wake_up_interruptible(&cb->rbh.sem);
      break;
    default:
      printk("rdmabox[%s]: client receives unknown msg\n",__func__);
      return -1; 	
  }
  return 0;
}

static int send_done_postwork(struct kernel_cb *cb)
{
  if (cb->state < CONNECTED){
    printk("rdmabox[%s]: cb is not connected\n",__func__);	
    return -1;
  }
  switch(cb->send_buf.type){
    case DELETE_RB:
      cb->rbh.c_state = RBH_DELETE_RB;
      wake_up_interruptible(&cb->rbh.sem);
      break;
    default:
      ; 	
  }

  return 0;	
}

static int read_done_postwork(struct kernel_cb * cb, struct ib_wc *wc)
{
  struct rdma_ctx *ctx;
  struct request *req;
  int i;

  ctx = (struct rdma_ctx *)ptr_from_uint64(wc->wr_id);	
  atomic_set(&ctx->in_flight, CTX_IDLE);

  req = ctx->req;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
  memcpy(bio_data(req->bio), ctx->rdma_buf, VALET_PAGE_SIZE);
#else
  memcpy(req->buffer, ctx->rdma_buf, VALET_PAGE_SIZE);
#endif

  ctx->rb_index = -1;
  ctx->req = NULL;
  put_rdma_freepool(&ctx->cb->valet_sess->rdma_read_freepool,ctx);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 18, 0)
  blk_mq_end_request(req, 0);
#else
  blk_mq_end_io(req, 0);
#endif

  return 0;
}

static int write_done_postwork(struct kernel_cb * cb, struct ib_wc *wc)
{
  struct rdma_ctx *ctx=NULL;
  struct local_page_list *sending_item=NULL;
  size_t i;
  int err;
#ifdef DISKBACKUP
  int num_replica = NUM_REPLICA+1;
#else
  int num_replica = NUM_REPLICA;
#endif

  ctx = (struct rdma_ctx *)ptr_from_uint64(wc->wr_id);	
  if (ctx == NULL || ctx->rb == NULL){
    printk("rdmabox[%s]: ctx is null or ctx->rb is null  \n",__func__);
    return 1;
  }
  sending_item = ctx->sending_item;
  if (unlikely(!sending_item)){
    printk("rdmabox[%s]: sending item is null  \n",__func__);
    return 1;
  }

  atomic_inc(&sending_item->ref_count);

  valet_bitmap_group_set(ctx->rb->bitmap_g, ctx->offset, ctx->len);

  if(ctx->replica_index == 0 && atomic_read(&ctx->sending_item->ref_count) < num_replica )
     return 0;

  if(atomic_read(&ctx->sending_item->ref_count) >= num_replica){
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
              err = put_reclaimable_index(entry);
              if(err)
                  printk("rdmabox[%s]: fail to put reclaim list \n",__func__);
          }
       }//for
       put_free_item(sending_item);

       if(ctx->replica_index != 0){
           ctx->prime_ctx->sending_item = NULL;
           ctx->prime_ctx->rb_index = -1;
           ctx->prime_ctx->rb = NULL;
           atomic_set(&ctx->prime_ctx->in_flight, CTX_IDLE);
           put_rdma_freepool(&ctx->prime_ctx->cb->valet_sess->rdma_write_freepool,ctx->prime_ctx);
       }
  }

  ctx->sending_item = NULL;
  ctx->rb_index = -1;
  ctx->rb = NULL;
  atomic_set(&ctx->in_flight, CTX_IDLE);
  put_rdma_freepool(&ctx->cb->valet_sess->rdma_write_freepool,ctx);

  return 0;
}

static void rdma_cq_event_handler(struct ib_cq * cq, void *ctx)
{
  struct kernel_cb *cb=ctx;
  struct ib_wc wc;
  struct ib_recv_wr * bad_wr;
  int ret;
  
  if (cb->state == ERROR) {
    printk("rdmabox[%s]: cq completion in ERROR state\n",__func__);
    return;
  }

  ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);

  while ((ret = ib_poll_cq(cb->cq, 1, &wc)) == 1) {
    if (wc.status) {
      if (wc.status == IB_WC_WR_FLUSH_ERR) {
	printk("rdmabox[%s]: wc.status : IB_WC_WR_FLUSH_ERR\n",__func__);
	continue;
      } else {
	printk("rdmabox[%s]: cq completion failed with "
	    "wr_id %Lx status %d %s opcode %d vender_err %x\n",__func__,
	    wc.wr_id, wc.status, wc.opcode, wc.vendor_err);
	goto error;
      }
    }	
    switch (wc.opcode){
      case IB_WC_RECV:
	ret = recv_msg_handler(cb, &wc);
	if (ret) {
	  printk("rdmabox[%s]: recv wc error: %d\n",__func__, ret);
	  goto error;
	}
	ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret) {
	  printk("rdmabox[%s]: post recv error: %d\n",__func__,ret);
	  goto error;
	}
	if (cb->state == RECV_QUERY_REPL || cb->state == RECV_MR_MAP || cb->state == RECV_MIG_MAP){
	  wake_up_interruptible(&cb->sem);
	}
	break;
      case IB_WC_SEND:
	ret = send_done_postwork(cb);
	if (ret) {
	  printk("rdmabox[%s]: send wc error: %d\n",__func__, ret);
	  goto error;
	}
	break;
      case IB_WC_RDMA_READ:
	ret = read_done_postwork(cb, &wc);
	if (ret) {
	  printk("rdmabox[%s]: read wc error: %d, cb->state=%d\n",__func__, ret, cb->state);
	  goto error;
	}
	break;
      case IB_WC_RDMA_WRITE:
	ret = write_done_postwork(cb, &wc);
	if (ret) {
	  printk("rdmabox[%s]: write wc error: %d, cb->state=%d\n",__func__, ret, cb->state);
	  goto error;
	}
	break;
      default:
	printk("rdmabox[%s]: %d Unexpected opcode %d, Shutting down\n", __func__, __LINE__, wc.opcode);
	goto error;
    }
  }
  if (ret){
    printk("rdmabox[%s]: poll error %d\n",__func__, ret);
    goto error;
  }
  return;
error:
  cb->state = ERROR;
}

static void valet_setup_wr(struct kernel_cb *cb)
{
  cb->recv_sgl.addr = cb->recv_dma_addr;
  cb->recv_sgl.length = sizeof cb->recv_buf;
  //if (cb->local_dma_lkey)
  //  cb->recv_sgl.lkey = cb->qp->device->local_dma_lkey;
  //else if (cb->mem == DMA)
    cb->recv_sgl.lkey = cb->dma_mr->lkey;
  cb->rq_wr.sg_list = &cb->recv_sgl;
  cb->rq_wr.num_sge = 1;

  cb->send_sgl.addr = cb->send_dma_addr;
  cb->send_sgl.length = sizeof cb->send_buf;
  //if (cb->local_dma_lkey)
  //  cb->send_sgl.lkey = cb->qp->device->local_dma_lkey;
  //else if (cb->mem == DMA)
    cb->send_sgl.lkey = cb->dma_mr->lkey;
  cb->sq_wr.opcode = IB_WR_SEND;
  cb->sq_wr.send_flags = IB_SEND_SIGNALED;
  cb->sq_wr.sg_list = &cb->send_sgl;
  cb->sq_wr.num_sge = 1;
}

static int valet_setup_buffers(struct kernel_cb *cb, struct valet_session *valet_session)
{
  int ret;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)	
  cb->recv_dma_addr = dma_map_single(&valet_session->pd->device->dev, 
      &cb->recv_buf, sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
#else
  cb->recv_dma_addr = dma_map_single(valet_session->pd->device->dma_device, 
      &cb->recv_buf, sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
#endif
  pci_unmap_addr_set(cb, recv_mapping, cb->recv_dma_addr);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
  cb->send_dma_addr = dma_map_single(&valet_session->pd->device->dev, 
      &cb->send_buf, sizeof(cb->send_buf), DMA_BIDIRECTIONAL);	
#else
  cb->send_dma_addr = dma_map_single(valet_session->pd->device->dma_device, 
      &cb->send_buf, sizeof(cb->send_buf), DMA_BIDIRECTIONAL);
#endif
  pci_unmap_addr_set(cb, send_mapping, cb->send_dma_addr);
  //if (cb->mem == DMA) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
    cb->dma_mr = valet_session->pd->device->get_dma_mr(valet_session->pd, IB_ACCESS_LOCAL_WRITE|
 				        IB_ACCESS_REMOTE_READ|
				        IB_ACCESS_REMOTE_WRITE);
#else
    cb->dma_mr = ib_get_dma_mr(valet_session->pd, IB_ACCESS_LOCAL_WRITE|
	IB_ACCESS_REMOTE_READ|
	IB_ACCESS_REMOTE_WRITE);
#endif
    if (IS_ERR(cb->dma_mr)) {
      printk("rdmabox[%s]: reg_dmamr failed\n",__func__);
      
      ret = PTR_ERR(cb->dma_mr);
      goto bail;
    }
  //} 
  valet_setup_wr(cb);

  return 0;
bail:

  if (cb->dma_mr && !IS_ERR(cb->dma_mr))
    ib_dereg_mr(cb->dma_mr);
  if (cb->recv_mr && !IS_ERR(cb->recv_mr))
    ib_dereg_mr(cb->recv_mr);
  if (cb->send_mr && !IS_ERR(cb->send_mr))
    ib_dereg_mr(cb->send_mr);

  return ret;
}

static void valet_free_buffers(struct kernel_cb *cb)
{
  if (cb->dma_mr)
    ib_dereg_mr(cb->dma_mr);
  if (cb->send_mr)
    ib_dereg_mr(cb->send_mr);
  if (cb->recv_mr)
    ib_dereg_mr(cb->recv_mr);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)	
    dma_unmap_single(&cb->valet_sess->pd->device->dev,
		 pci_unmap_addr(cb, recv_mapping),
      	         sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
    dma_unmap_single(&cb->valet_sess->pd->device->dev,
   	         pci_unmap_addr(cb, send_mapping),
		 sizeof(cb->send_buf), DMA_BIDIRECTIONAL);
#else
    dma_unmap_single(cb->valet_sess->pd->device->dma_device,
		 pci_unmap_addr(cb, recv_mapping),
		 sizeof(cb->recv_buf), DMA_BIDIRECTIONAL);
    dma_unmap_single(cb->valet_sess->pd->device->dma_device,
		 pci_unmap_addr(cb, send_mapping),
 		 sizeof(cb->send_buf), DMA_BIDIRECTIONAL);
#endif
}

static int valet_create_qp(struct kernel_cb *cb, struct valet_session *valet_session)
{
  struct ib_qp_init_attr init_attr;
  int ret;

  memset(&init_attr, 0, sizeof(init_attr));
  init_attr.cap.max_send_wr = cb->txdepth;
  init_attr.cap.max_recv_wr = cb->txdepth;  
  init_attr.cap.max_recv_sge = 1;
  init_attr.cap.max_send_sge = 1;
  init_attr.qp_type = IB_QPT_RC;
  init_attr.send_cq = cb->cq;
  init_attr.recv_cq = cb->cq;
  init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;

  ret = rdma_create_qp(cb->cm_id, valet_session->pd, &init_attr);
  if (!ret)
    cb->qp = cb->cm_id->qp;
  return ret;
}

static void valet_free_qp(struct kernel_cb *cb)
{
  ib_destroy_qp(cb->qp);
  ib_destroy_cq(cb->cq);
}

static int valet_setup_qp(struct kernel_cb *cb, struct rdma_cm_id *cm_id, struct valet_session *valet_session)
{
  int ret;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
  struct ib_cq_init_attr init_attr;
#endif

    if(!valet_session->pd){
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
        valet_session->pd = ib_alloc_pd(cm_id->device, IB_ACCESS_LOCAL_WRITE|
                                        IB_ACCESS_REMOTE_READ|
                                        IB_ACCESS_REMOTE_WRITE );
#else
        valet_session->pd = ib_alloc_pd(cm_id->device);
#endif
        if (IS_ERR(valet_session->pd)) {
            printk("rdmabox[%s]: ib_alloc_pd failed\n",__func__);
            return PTR_ERR(valet_session->pd);
        }
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
    memset(&init_attr, 0, sizeof(init_attr));
    init_attr.cqe = cb->txdepth * 2;
    init_attr.comp_vector = 0;
				
    cb->cq = ib_create_cq(cm_id->device, rdma_cq_event_handler, NULL, cb, &init_attr);
#else
    cb->cq = ib_create_cq(cm_id->device, rdma_cq_event_handler, NULL, cb, cb->txdepth * 2, 0);
#endif
  if (IS_ERR(cb->cq)) {
    printk("rdmabox[%s]: ib_create_cq failed\n",__func__);
    
    ret = PTR_ERR(cb->cq);
    goto err1;
  }
  ret = ib_req_notify_cq(cb->cq, IB_CQ_NEXT_COMP);
  if (ret) {
    printk("rdmabox[%s]: ib_create_cq failed\n",__func__);
    
    goto err2;
  }

  ret = valet_create_qp(cb, valet_session);
  if (ret) {
    printk("rdmabox[%s]: valet_create_qp failed: %d\n",__func__, ret);
    
    goto err2;
  }
  return 0;
err2:
  ib_destroy_cq(cb->cq);
err1:
  return ret;
}

static void fill_sockaddr(struct sockaddr_storage *sin, struct kernel_cb *cb)
{
  memset(sin, 0, sizeof(*sin));

  if (cb->addr_type == AF_INET) {
    struct sockaddr_in *sin4 = (struct sockaddr_in *)sin;
    sin4->sin_family = AF_INET;
    memcpy((void *)&sin4->sin_addr.s_addr, cb->addr, 4);
    sin4->sin_port = cb->port;
  } else if (cb->addr_type == AF_INET6) {
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)sin;
    sin6->sin6_family = AF_INET6;
    memcpy((void *)&sin6->sin6_addr, cb->addr, 16);
    sin6->sin6_port = cb->port;
  }
}

static int valet_connect_client(struct kernel_cb *cb)
{
  struct rdma_conn_param conn_param;
  int ret;
  int tmp_cb_index = -1;

  memset(&conn_param, 0, sizeof conn_param);
  conn_param.responder_resources = 1;
  conn_param.initiator_depth = 1;
  conn_param.retry_count = 10;
  conn_param.private_data = (int*)&tmp_cb_index; // mark for client cb connection
  conn_param.private_data_len = sizeof(int);

  ret = rdma_connect(cb->cm_id, &conn_param);
  if (ret) {
    printk("rdmabox[%s]: rdma_connect error %d\n",__func__, ret);
    
    return ret;
  }

  wait_event_interruptible(cb->sem, cb->state >= CONNECTED);
  if (cb->state == ERROR) {
    printk("rdmabox[%s]: wait for CONNECTED state %d\n",__func__, cb->state);
    
    return -1;
  }

  return 0;
}

static int valet_bind_client(struct kernel_cb *cb)
{
  struct sockaddr_storage sin;
  int ret;

  fill_sockaddr(&sin, cb);

  ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *)&sin, 2000);
  if (ret) {
    printk("rdmabox[%s]: rdma_resolve_addr error %d\n",__func__, ret);
    
    return ret;
  }

  wait_event_interruptible(cb->sem, cb->state >= ROUTE_RESOLVED);
  if (cb->state != ROUTE_RESOLVED) {
    printk("rdmabox[%s]: addr/route resolution did not resolve: state %d\n",__func__,cb->state);
    
    return -EINTR;
  }
  return 0;
}

const char *valet_device_state_str(struct valet_file *dev)
{
  char *state;

  spin_lock(&dev->state_lock);
  switch (dev->state) {
    case DEVICE_INITIALIZING:
      state = "Initial state";
      break;
    case DEVICE_OPENNING:
      state = "openning";
      break;
    case DEVICE_RUNNING:
      state = "running";
      break;
    case DEVICE_OFFLINE:
      state = "offline";
      break;
    default:
      state = "unknown device state";
  }
  spin_unlock(&dev->state_lock);

  return state;
}

static int connect_remote(struct kernel_cb *cb, struct valet_session *valet_session)
{
  struct ib_recv_wr *bad_wr;
  int ret;

  ret = valet_bind_client(cb);
  if (ret)
    return ret;

  ret = valet_setup_qp(cb, cb->cm_id, valet_session);
  if (ret) {
    printk("rdmabox[%s]: setup_qp failed: %d\n",__func__, ret);
    
    return ret;
  }

  ret = valet_setup_buffers(cb, valet_session);
  if (ret) {
    printk("rdmabox[%s]: valet_setup_buffers failed: %d\n",__func__, ret);
    
    goto err1;
  }

  ret = ib_post_recv(cb->qp, &cb->rq_wr, &bad_wr); 
  if (ret) {
    printk("rdmabox[%s]: ib_post_recv failed: %d\n",__func__, ret);
    
    goto err2;
  }

  ret = valet_connect_client(cb);  
  if (ret) {
    printk("rdmabox[%s]: connect error %d\n",__func__, ret);
    goto err2;
  }

  return 0;

err2:
  valet_free_buffers(cb);
  return ret;

err1:
  valet_free_qp(cb);	
  return ret;
}

static int control_block_init(struct kernel_cb *cb, struct valet_session *valet_session)
{
  int ret = 0;
  int i;

  cb->valet_sess = valet_session;
  cb->addr_type = AF_INET;
  //cb->mem = DMA;
  cb->txdepth = VALET_QUEUE_DEPTH * submit_queues + 1;
  cb->state = IDLE;

  cb->rbh.block_size_gb = 0;
  cb->rbh.remote_block = (struct remote_block **)kzalloc(sizeof(struct remote_block *) * MAX_MR_SIZE_GB, GFP_KERNEL);
  cb->rbh.remote_mapped = (atomic_t *)kmalloc(sizeof(atomic_t) * MAX_MR_SIZE_GB, GFP_KERNEL);
  cb->rbh.rb_idx_map = (int *)kzalloc(sizeof(int) * MAX_MR_SIZE_GB, GFP_KERNEL);
  cb->rbh.mig_rb_idx = (char *)kzalloc(sizeof(char) * MAX_MR_SIZE_GB, GFP_KERNEL);
  for (i=0; i < MAX_MR_SIZE_GB; i++){
    atomic_set(cb->rbh.remote_mapped + i, RB_UNMAPPED);
    cb->rbh.rb_idx_map[i] = -1;
    cb->rbh.remote_block[i] = (struct remote_block *)kzalloc(sizeof(struct remote_block), GFP_KERNEL); 
    cb->rbh.remote_block[i]->rb_state = ACTIVE; 
    cb->rbh.mig_rb_idx[i] = 0x00;
  }

  init_waitqueue_head(&cb->sem);
  init_waitqueue_head(&cb->rbh.sem);
  cb->rbh.c_state = RBH_IDLE;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
  	cb->cm_id = rdma_create_id(&init_net, valet_cma_event_handler, cb, RDMA_PS_TCP, IB_QPT_RC);
#else
	cb->cm_id = rdma_create_id(valet_cma_event_handler, cb, RDMA_PS_TCP, IB_QPT_RC);
#endif
  if (IS_ERR(cb->cm_id)) {
    ret = PTR_ERR(cb->cm_id);
    printk("rdmabox[%s]: rdma_create_id error %d\n",__func__, ret);
    
    goto out;
  } 
  return 0;
out:
  kfree(cb);
  return ret;
}

int find_the_best_node(struct valet_session *valet_session, int *replica_list, int num_replica, int isMig){

  int i, j;
  unsigned int k;
  int num_possible_servers=0;
  int num_available_servers=0;
  struct kernel_cb *tmp_cb;
  int candidate[valet_session->cb_num];
  unsigned int select_history[valet_session->cb_num];
  unsigned int cb_index;

  int free_mem[SERVER_SELECT_NUM];
  int free_mem_sorted[SERVER_SELECT_NUM]; 
  int tmp_int;
  unsigned long tmp_ul;
  // initialization
  for (i = 0; i < SERVER_SELECT_NUM; i++){
    free_mem[i] = -1;
    free_mem_sorted[i] = valet_session->cb_num;
  }
  for (i=0; i < valet_session->cb_num ;i++){
    candidate[i] = -1;
    select_history[i] = 0;
  }

  // find out available servers
  for (i=0; i<valet_session->cb_num;i++){
    if (valet_session->cb_state_list[i] < CB_MIGRATING) {
      candidate[num_possible_servers]=i;
      num_possible_servers++;
    }
  }

  // select random nodes
  for (i=0; i < num_possible_servers ;i++){
      
      if(num_possible_servers <= SERVER_SELECT_NUM){
          cb_index = candidate[i];
      }else{
          get_random_bytes(&k, sizeof(unsigned int));
          k %= num_possible_servers;
          while (select_history[k] == 1) {
              k += 1;	
              k %= num_possible_servers;
          }
          select_history[k] = 1;
          cb_index = candidate[k];
      }

      tmp_cb = valet_session->cb_list[cb_index];
      if (valet_session->cb_state_list[cb_index] > CB_IDLE) {
          tmp_cb->send_buf.type = REQ_QUERY;
          post_send(tmp_cb);
          wait_event_interruptible(tmp_cb->sem, tmp_cb->state == RECV_QUERY_REPL);
      }else { // cb_idle
          control_block_init(tmp_cb, valet_session);
          connect_remote(tmp_cb, valet_session);	
          wait_event_interruptible(tmp_cb->sem, tmp_cb->state == RECV_QUERY_REPL);
          valet_session->cb_state_list[cb_index] = CB_CONNECTED; //add cb_connected		
      }

      if(tmp_cb->rbh.target_size_gb == 0){
	 continue;
      }

      free_mem[num_available_servers] = tmp_cb->rbh.target_size_gb;
      free_mem_sorted[num_available_servers] = cb_index;
      num_available_servers++;
      if(num_available_servers == SERVER_SELECT_NUM)
	  break;
  }//for

  // remote nodes have not enough free_mem
  if(num_available_servers < num_replica)
      return 0;

  // do sorting descent order of freemem 
  for (i=0; i<num_available_servers-1; i++) {
    for (j=i+1; j<num_available_servers; j++) {
      if (free_mem[i] < free_mem[j]) {
	tmp_int = free_mem[i];
        free_mem[i] = free_mem[j];
        free_mem[j] = tmp_int;
        tmp_int = free_mem_sorted[i];
        free_mem_sorted[i] = free_mem_sorted[j];
        free_mem_sorted[j] = tmp_int;
      }
    }
  }
  j = 0;
  for(i=0; i<num_replica; i++){
      replica_list[j] = free_mem_sorted[i];
      printk("rdmabox[%s]: selected node : replica[%d]=%d \n",__func__,j,replica_list[j]);
      j++;
  }

  return j;
}

void post_setup_connection(struct valet_session *valet_session, struct kernel_cb *tmp_cb, int cb_index)
{
    char name[2];

    if(!valet_session->rdma_write_freepool.init_done)
        rdma_freepool_post_init(valet_session, tmp_cb, &valet_session->rdma_write_freepool, VALET_PAGE_SIZE * RDMA_WR_BUF_LEN, 1);
    if(!valet_session->rdma_read_freepool.init_done)
        rdma_freepool_post_init(valet_session, tmp_cb, &valet_session->rdma_read_freepool, VALET_PAGE_SIZE, 0);

    memset(name, '\0', 2);
    name[0] = (char)((cb_index/26) + 97);
    tmp_cb->rbh.mig_thread = kthread_create(migration_handler, tmp_cb, name);
    wake_up_process(tmp_cb->rbh.mig_thread);	
}

int valet_remote_block_map(struct valet_session *valet_session, int select_idx, struct kernel_cb **replica_cb_list)
{
  struct kernel_cb *tmp_cb;
  int cb_index;
  int replica_list[NUM_REPLICA];
  int rst=0;
  int i;
  rst = find_the_best_node(valet_session,replica_list,NUM_REPLICA,0);
  if(rst < NUM_REPLICA){
      printk("rdmabox[%s]: fail to find_the_best_node num_found:%d\n",__func__,rst);
      return rst;
  }

  rst=0;
  for(i=0; i<NUM_REPLICA; i++){
      cb_index = replica_list[i];
      tmp_cb = valet_session->cb_list[cb_index];
      if (valet_session->cb_state_list[cb_index] == CB_CONNECTED){ 
          post_setup_connection(valet_session, tmp_cb, cb_index);
      }
      valet_session->mapped_cb_num += 1;

      tmp_cb->send_buf.type = REQ_MR_MAP;
      tmp_cb->send_buf.size_gb = select_idx; 
      tmp_cb->send_buf.replica_index = i; //use this for marking for replica index
      post_send(tmp_cb);

      wait_event_interruptible(tmp_cb->sem, tmp_cb->state == RECV_MR_MAP);
      atomic_set(&valet_session->rdma_on, DEV_RDMA_ON); 

      replica_cb_list[i] = tmp_cb;
      rst++;
  }//replica loop

  //valet_session->pos_wr_hist = (++valet_session->pos_wr_hist)%3;
  //valet_session->pos_rd_hist = (++valet_session->pos_rd_hist)%3;

  return rst;
}

struct kernel_cb* valet_migration_block_map(struct valet_session *valet_session, int client_evict_rb_index, struct kernel_cb *old_cb, int src_cb_index, int replica_index)
{
  struct kernel_cb *tmp_cb;
  int cb_index;
  int i;
  int src_rb_index;
  int replica_list[NUM_REPLICA];
  int rst=0;

  rst = find_the_best_node(valet_session,replica_list,1,1);
  if(rst < 1){
      printk("rdmabox[%s]: fail to find_the_best_node num_found:%d\n",__func__,rst);
      return NULL;
  }

  rst=0;
  cb_index = replica_list[0];

  src_rb_index = valet_session->mapping_swapspace_to_rb[replica_index][client_evict_rb_index];

  tmp_cb = valet_session->cb_list[cb_index];
  if (valet_session->cb_state_list[cb_index] == CB_CONNECTED){ 
      post_setup_connection(valet_session, tmp_cb, cb_index);
  }

  printk("rdmabox[%s]: Send bind migration MSG for swapspace[%d](rb[%d] on srcnode[%d]) to node[%d] \n",__func__,client_evict_rb_index,src_rb_index,src_cb_index,cb_index);

  // evict addr and rkey at src addr
  for (i=0; i<MAX_MR_SIZE_GB; i++){
      if (old_cb->rbh.mig_rb_idx[i] == 'm'){
          tmp_cb->send_buf.rkey[i] = old_cb->rbh.remote_block[i]->remote_rkey;
          tmp_cb->send_buf.buf[i] = old_cb->rbh.remote_block[i]->remote_addr;
	  break;
      }else{
          tmp_cb->send_buf.rkey[i] = 0;
      }
  }

  tmp_cb->send_buf.type = REQ_MIG_MAP;
  tmp_cb->send_buf.size_gb = client_evict_rb_index;
  tmp_cb->send_buf.rb_index = src_rb_index;
  tmp_cb->send_buf.cb_index = src_cb_index;
  tmp_cb->send_buf.replica_index = replica_index;
  post_send(tmp_cb);

  wait_event_interruptible(tmp_cb->sem, tmp_cb->state == RECV_MIG_MAP);
  //valet_session->pos_wr_hist = (++valet_session->pos_wr_hist)%3;
  //valet_session->pos_rd_hist = (++valet_session->pos_rd_hist)%3;

  return tmp_cb;
}

static void portal_parser(struct valet_session *valet_session)
{
  char *ptr = valet_session->portal + 7;   //rdma://[]
  char *single_portal = NULL;
  int p_count=0, i=0, j=0;
  int port = 0;

  sscanf(strsep(&ptr, ","), "%d", &p_count);
  valet_session->cb_num = p_count;
  valet_session->portal_list = kzalloc(sizeof(struct valet_portal) * valet_session->cb_num, GFP_KERNEL); 

  for (i=0; i < p_count; i++){
    single_portal = strsep(&ptr, ",");

    j = 0;
    while (*(single_portal + j) != ':'){
      j++;
    }
    memcpy(valet_session->portal_list[i].addr, single_portal, j);
    valet_session->portal_list[i].addr[j] = '\0';
    port = 0;
    sscanf(single_portal+j+1, "%d", &port);
    valet_session->portal_list[i].port = (uint16_t)port; 
    printk("rdmabox[%s]: portal: %s, %d\n",__func__, valet_session->portal_list[i].addr, valet_session->portal_list[i].port);
    
  }     
}

int valet_session_create(const char *portal, struct valet_session *valet_session)
{
  int i, j, ret;
  char name[20];
  char thread_name[10]="rsender";

  printk("rdmabox[%s]: In valet_session_create() with portal: %s\n",__func__, portal);

  memcpy(valet_session->portal, portal, strlen(portal));
  printk("rdmabox[%s]: %s\n",__func__,valet_session->portal);

  spin_lock_init(&valet_session->cb_lock);
  
  portal_parser(valet_session);

  //valet_session->pos_wr_hist = 0; 
  //valet_session->pos_rd_hist = 0; 
  valet_session->capacity_g = SWAPSPACE_SIZE_G; 
  valet_session->capacity = (unsigned long long)SWAPSPACE_SIZE_G * ONE_GB;
  valet_session->mapped_cb_num = 0;
  valet_session->mapped_capacity = 0;
  valet_session->pd = NULL; 

  valet_session->cb_list = (struct kernel_cb **)kzalloc(sizeof(struct kernel_cb *) * valet_session->cb_num, GFP_KERNEL);	
  valet_session->cb_state_list = (enum cb_state *)kzalloc(sizeof(enum cb_state) * valet_session->cb_num, GFP_KERNEL);
  for (i=0; i<valet_session->cb_num; i++) {
    valet_session->cb_state_list[i] = CB_IDLE;	
    valet_session->cb_list[i] = kzalloc(sizeof(struct kernel_cb), GFP_KERNEL);
    valet_session->cb_list[i]->port = htons(valet_session->portal_list[i].port);
    in4_pton(valet_session->portal_list[i].addr, -1, valet_session->cb_list[i]->addr, -1, NULL);
    valet_session->cb_list[i]->cb_index = i;
  }//for

  for (i = 0; i < NUM_REPLICA; i++) {
      valet_session->cb_index_map[i] = kzalloc(sizeof(atomic_t) * valet_session->capacity_g, GFP_KERNEL);
      valet_session->mapping_swapspace_to_rb[i] = (int*)kzalloc(sizeof(int) * valet_session->capacity_g, GFP_KERNEL);
      for (j = 0; j < valet_session->capacity_g; j++){
          atomic_set(valet_session->cb_index_map[i] + j, NO_CB_MAPPED);
          valet_session->mapping_swapspace_to_rb[i][j] = -1;
      }
  }

  atomic_set(&valet_session->rdma_on, DEV_RDMA_OFF);

  if(rdma_freepool_create(valet_session, &valet_session->rdma_write_freepool, (submit_queues*VALET_QUEUE_DEPTH*valet_session->cb_num), VALET_PAGE_SIZE * RDMA_WR_BUF_LEN, 1)){
	panic("fail to create rdma write freepool");
  }
  if(rdma_freepool_create(valet_session, &valet_session->rdma_read_freepool, (submit_queues*VALET_QUEUE_DEPTH*valet_session->cb_num), VALET_PAGE_SIZE, 0)){
	panic("fail to create rdma read freepool");
  }

  return 0;

err_destroy_portal:

  return ret;
}

void valet_session_destroy(struct valet_session *valet_session)
{
  mutex_lock(&g_lock);
  list_del(&valet_session->list);
  mutex_unlock(&g_lock);
  
  valet_destroy_session_devices(valet_session);
}

/* from Infiniswap */
void valet_bitmap_set(int *bitmap, int i)
{
  bitmap[i >> BITMAP_SHIFT] |= 1 << (i & BITMAP_MASK);
}

void valet_bitmap_group_set(int *bitmap, unsigned long offset, unsigned long len)
{
  int start_page = (int)(offset/VALET_PAGE_SIZE);
  int len_page = (int)(len/VALET_PAGE_SIZE);
  int i;
  for (i=0; i<len_page; i++){
    valet_bitmap_set(bitmap, start_page + i);
  }
}
void valet_bitmap_group_clear(int *bitmap, unsigned long offset, unsigned long len)
{
  int start_page = (int)(offset/VALET_PAGE_SIZE);
  int len_page = (int)(len/VALET_PAGE_SIZE);
  int i;
  for (i=0; i<len_page; i++){
    valet_bitmap_clear(bitmap, start_page + i);
  }
}
bool valet_bitmap_test(int *bitmap, int i)
{
  if ((bitmap[i >> BITMAP_SHIFT] & (1 << (i & BITMAP_MASK))) != 0){
    return true;
  }else{
    return false;
  }
}
void valet_bitmap_clear(int *bitmap, int i)
{
  bitmap[i >> BITMAP_SHIFT] &= ~(1 << (i & BITMAP_MASK));
}
void valet_bitmap_init(int *bitmap)
{
  memset(bitmap, 0x00, ONE_GB/(4096*8));
}


