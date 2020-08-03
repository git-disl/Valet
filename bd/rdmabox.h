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

#ifndef RDMABOX_H
#define RDMABOX_H

#include <linux/init.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/parser.h>
#include <linux/proc_fs.h>
#include <linux/inet.h>
#include <linux/in.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/time.h>
#include <linux/random.h>
#include <linux/sched.h>
#include <linux/proc_fs.h>
#include <asm/pci.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

#include "valet_drv.h"

/* number of 4KB pages */

//  server selection, call m server each time.
// SERVER_SELECT_NUM should be >= NUM_REPLICA
#define SERVER_SELECT_NUM 2
#define NUM_REPLICA 1

#define FREE_MEM_WEIGHT 2
#define AGE_WEIGHT 1

//max_size from one server or max_size one server can provide
#define MAX_MR_SIZE_GB 32

#define MAX_PORTAL_NAME   2048

#define TRIGGER_ON 1
#define TRIGGER_OFF 0

#define NO_CB_MAPPED -1

#define RB_MAPPED 1
#define RB_UNMAPPED 0

#define CTX_IDLE	0
#define CTX_R_IN_FLIGHT	1
#define CTX_W_IN_FLIGHT	2

// added for RDMA_CONNECTION failure handling.
#define DEV_RDMA_ON		1
#define DEV_RDMA_OFF	0

//bitmap
//#define INT_BITS 32
#define BITMAP_SHIFT 5 // 2^5=32
#define ONE_GB_SHIFT 30
#define BITMAP_MASK 0x1f // 2^5=32
#define ONE_GB_MASK 0x3fffffff
#define ONE_GB 1073741824 //1024*1024*1024 
#define BITMAP_INT_SIZE 8192 //bitmap[], 1GB/4k/32

extern struct list_head g_valet_sessions;

struct portal {
        uint8_t addr[16];                  
        uint16_t port;               
};

enum msg_opcode{
    DONE = 1,
    RESP_MR_MAP,
    RESP_MIG_MAP,
    REQ_QUERY,
    RESP_QUERY,
    EVICT,
    REQ_MR_MAP,
    REQ_MIG_MAP,
    REQ_MIGRATION,
    DONE_MIGRATION,
    DELETE_RB
};

enum remote_block_handler_state {
    RBH_IDLE,
    RBH_READY,
    RBH_EVICT,
    RBH_MIGRATION_READY,
    RBH_MIGRATION_DONE,
    RBH_DELETE_RB,
};

enum test_state {
	IDLE = 1,
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED,		
	RECV_QUERY_REPL,
	RECV_MR_MAP,
	RECV_EVICT,
        RECV_MIG_MAP,
        RECV_MIGRATION_DONE,
	CM_DISCONNECT,
	ERROR
};

struct message {
	enum msg_opcode type;
  	int size_gb;
        int rb_index;
        int cb_index;
        int replica_index;
  	uint64_t buf[MAX_MR_SIZE_GB];
  	uint32_t rkey[MAX_MR_SIZE_GB];
};

struct query_message{
  enum msg_opcode type;
  int size_gb;
  unsigned long sum;
};


enum remote_block_state {
       ACTIVE=1,
       READONLY,
       NONE_ALLOWED,
       INACTIVE
};
        
struct remote_block_header {
       struct timeval *tv;
       enum remote_block_state state;
       u8 addr[16];
       u8 reserve[4064];
};

struct remote_block {
	uint32_t remote_rkey;	
	uint64_t remote_addr;
        enum remote_block_state rb_state;
	int *bitmap_g;	
	int replica_index;
};

struct remote_block_handler {
	struct remote_block **remote_block;
	atomic_t *remote_mapped; //atomic_t type * MAX_MR_SIZE_GB

	int block_size_gb; //size = num block * ONE_GB
	int target_size_gb; // freemem from server
        unsigned long sum; // sum of age from server

	struct task_struct *mig_thread;
	int *rb_idx_map;	//block_index to swapspace index
	char *mig_rb_idx;
	wait_queue_head_t sem;      	
	enum remote_block_handler_state c_state;

        int dest_rb_index;
        int dest_cb_index;
        int src_rb_index;
        int src_cb_index;
        int client_swapspace_index; 
        int replica_index;
};


typedef struct rdma_freepool_s {
        spinlock_t rdma_freepool_lock;
        size_t cap_nr;          /* capacity. nr of elements at *elements */
        size_t curr_nr;         /* current nr of elements at *elements */
        void **elements;
        int init_done;
}rdma_freepool_t;

struct kernel_cb {
	int cb_index; //index in valet_sess->cb_list
	struct valet_session *valet_sess;
	int server;			/* 0 iff client */
	struct ib_cq *cq;
	struct ib_pd *pd;
	struct ib_qp *qp;

	struct ib_mr *dma_mr;

	// memory region
	struct ib_recv_wr rq_wr;
	struct ib_sge recv_sgl;		
	struct message recv_buf;
	u64 recv_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(recv_mapping)
	struct ib_mr *recv_mr;

	struct ib_send_wr sq_wr;
	struct ib_sge send_sgl;
	struct message send_buf;
	u64 send_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(send_mapping)
	struct ib_mr *send_mr;

	struct remote_block_handler rbh;

	enum test_state state;		/* used for cond/signalling */
	wait_queue_head_t sem;      
	wait_queue_head_t write_sem;   

	u8 addr[16];			
	uint16_t port;	
	uint8_t addr_type;		/* ADDR_FAMILY - IPv4/V6 */
	int verbose;	
	int size;	
	int txdepth;		
	int local_dma_lkey;	

	/* CM stuff  connection management*/
	struct rdma_cm_id *cm_id;	
	struct rdma_cm_id *child_cm_id;	

	struct list_head list;	
};

struct rdma_ctx {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
        struct ib_rdma_wr rdma_sq_wr[2];        /* rdma work request record */
        struct ib_rdma_wr rdma_sq_rd;   /* rdma work request record */
#else
        struct ib_send_wr rdma_sq_wr[2];        /* rdma work request record */
        struct ib_send_wr rdma_sq_rd;   /* rdma work request record */
#endif
        struct ib_sge rdma_sgl_wr[2];           /* rdma single SGE */
        struct ib_sge rdma_sgl_rd;              /* rdma single SGE */

        char *header_buf;                       /* used as rdma sink for header */
        char *rdma_buf;                 /* used as rdma sink for remote block */
        u64  header_dma_addr;
        DECLARE_PCI_UNMAP_ADDR(header_mapping)
	u64  rdma_dma_addr;
	DECLARE_PCI_UNMAP_ADDR(rdma_mapping)
	struct ib_mr *rdma_mr;

	struct request *req;
	int rb_index;
	struct kernel_cb *cb;
	unsigned long offset;
	unsigned long len;
	struct remote_block *rb;
	struct local_page_list *sending_item;
        atomic_t in_flight; //true = 1, false = 0
        int replica_index;
        struct rdma_ctx *prime_ctx;
#ifdef LAT_DEBUG
        struct timeval starttime;
#endif
};

struct valet_portal {
	uint16_t port;		
	u8 addr[16];			
};
enum cb_state {
	CB_IDLE=0,
	CB_CONNECTED,	//connected but not mapped 
	CB_MAPPED,
	CB_MIGRATING,
	CB_EVICTING,
	CB_FAIL
};

struct valet_session {
	int cb_num;	//num of servers
	int mapped_cb_num;	//How many cbs are remote mapped
	struct kernel_cb	**cb_list;	
	struct valet_portal *portal_list;
	enum cb_state *cb_state_list; //all cbs state: not used, connected, failure

	struct valet_file 		*xdev;	// each session only creates a single valet_file

	char			      portal[MAX_PORTAL_NAME];

	struct list_head	      list;
	struct list_head	      devs_list; /* list of struct valet_file */
	spinlock_t		      devs_lock;
        spinlock_t                    cb_lock;
	struct config_group	      session_cg;
	struct completion	      conns_wait;
	atomic_t		      conns_count;
	atomic_t		      destroy_conns_count;

	unsigned long long    capacity;
	unsigned long long 	  mapped_capacity;
	int 	capacity_g;

	atomic_t *cb_index_map[NUM_REPLICA];  //unmapped==-1, this rb is mapped to which cb
	int *mapping_swapspace_to_rb[NUM_REPLICA];
	atomic_t	rdma_on;	//DEV_RDMA_ON/OFF

	struct ib_pd *pd;

        rdma_freepool_t rdma_write_freepool;
        rdma_freepool_t rdma_read_freepool;

/*
        //swap space ops counter
        int pos_wr_hist;
        int pos_rd_hist;
        unsigned long wr_ops[SWAPSPACE_SIZE_G][3];
        unsigned long rd_ops[SWAPSPACE_SIZE_G][3];
*/
};

int isRDMAon(struct valet_session *valet_sess);
void put_rdma_freepool(rdma_freepool_t *pool, void *element);
void *get_rdma_freepool(rdma_freepool_t *pool);

int request_write(struct valet_session *valet_sess, struct local_page_list *tmp);
int request_read(struct valet_session *valet_sess, struct request *req);

void valet_remote_block_init(struct kernel_cb *cb);
int valet_remote_block_map(struct valet_session *valet_session, int select_idx, struct kernel_cb **replica_cb_list);
struct kernel_cb* valet_migration_block_map(struct valet_session *valet_session, int client_evict_rb_index, struct kernel_cb *old_cb, int src_cb_index, int replica_index);

int valet_session_create(const char *portal, struct valet_session *valet_session);
void valet_session_destroy(struct valet_session *valet_session);

const char* valet_device_state_str(struct valet_file *dev);
int valet_set_device_state(struct valet_file *dev, enum valet_dev_state state);

void valet_bitmap_set(int *bitmap, int i);
bool valet_bitmap_test(int *bitmap, int i);
void valet_bitmap_clear(int *bitmap, int i);
void valet_bitmap_init(int *bitmap);
void valet_bitmap_group_set(int *bitmap, unsigned long offset, unsigned long len);
void valet_bitmap_group_clear(int *bitmap, unsigned long offset, unsigned long len);

#endif  /* RDMABOX_H */

