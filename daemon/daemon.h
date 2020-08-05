/*
 * Valet: Efficient Orchestration of Host and Remote Shared Memory
 *        for Memory IntensiveWorkloads
 * Copyright 2020 Georgia Institute of Technology.
 *
 * Copyright (c) 2005 Ammasso, Inc. All rights reserved.
 * Copyright (c) 2006 Open Grid Computing, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef RPING_H
#define RPING_H

#include <stdbool.h>
#include <getopt.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <pthread.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <semaphore.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <linux/kernel.h>

#define MAX_CLIENT      32

//for c320
//#define MAX_FREE_MEM_GB 8 //for local memory management
//#define MAX_MR_SIZE_GB 8 //for msg passing

#define MAX_FREE_MEM_GB 32 //for local memory management
#define MAX_MR_SIZE_GB 32 //for msg passing

#define ONE_MB 1048576
#define ONE_GB 1073741824
#define HALF_ONE_GB 536870912
//for c320
//#define FREE_MEM_EVICT_THRESHOLD 4 //in GB
//#define FREE_MEM_EXPAND_THRESHOLD 8 // in GB
#define FREE_MEM_EVICT_THRESHOLD 10 //in GB
#define FREE_MEM_EXPAND_THRESHOLD 20 // in GB

#define CURR_FREE_MEM_WEIGHT 0.7
#define MEM_EVICT_HIT_THRESHOLD 1
#define MEM_EXPAND_HIT_THRESHOLD 20

#define RB_MALLOCED 1
#define RB_EMPTY     0

#define TEST_NZ(x) do { if ( (x)) die("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_Z(x)  do { if (!(x)) die("error: " #x " failed (returned zero/null)."); } while (0)

#define ntohll(x) (((uint64_t)(ntohl((int)((x << 32) >> 32))) << 32) | \
        (unsigned int)ntohl(((int)(x >> 32))))

int debug = 1;
#define DEBUG_LOG if (debug) printf

struct portal {
        uint8_t addr[16];                    /* dst addr in NBO */
        uint16_t port;                  /* dst port in NBO */
};

enum cb_state{
  CB_IDLE,
  CB_CONNECTED,
  CB_MAPPED,
  CB_FAILED
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

enum test_state {
	IDLE = 1,
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED,
        RECV_QUERY_REPL,
	RECV_MR_MAP,
	RECV_MIG_MAP,
        RDMA_READ_INP,
	RDMA_READ_COMPLETE,
        RDMA_WRITE_INP,
	RDMA_WRITE_COMPLETE,
	RDMA_SEND_COMPLETE,
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
        uint8_t addr[16];
        uint8_t reserve[4064];
};

#define RPING_SQ_DEPTH 16351
#define RPING_RQ_DEPTH 10240

#define _stringify( _x ) # _x
#define stringify( _x ) _stringify( _x )

#define RPING_MSG_FMT           "rdma-ping-%d: "
#define RPING_MIN_BUFSIZE       sizeof(stringify(INT_MAX)) + sizeof(RPING_MSG_FMT)

struct migration_remote_block_handler {
  uint64_t buf[MAX_MR_SIZE_GB];
  uint32_t rkey[MAX_MR_SIZE_GB];
  int client_rb_index;
  int client_cb_index;
  int src_rb_index;
  int src_cb_index;
  int local_rb_index;
  pthread_t migthread;
  sem_t migration_sem;
};
/*
 * Control block struct.
 */
struct rping_cb {
	int server;			/* 0 iff client */
        int cb_index;
	pthread_t send_cq_thd; // per connection
	struct ibv_comp_channel *channel;
	struct ibv_cq *cq;
	struct ibv_pd *pd;
	struct ibv_qp *qp;

	struct ibv_recv_wr rq_wr;	/* recv work request record */
	struct ibv_sge recv_sgl;	/* recv single SGE */
	struct message *recv_buf;/* malloc'd buffer */
	struct ibv_mr *recv_mr;		/* MR associated with this buffer */

	struct ibv_send_wr sq_wr;	/* send work request record */
	struct ibv_sge send_sgl;
	struct query_message qmsg;/* single send buf */
	struct message *send_buf;/* single send buf */
	struct ibv_mr *send_mr;

	uint32_t remote_rkey;		/* remote guys RKEY */
	uint64_t remote_addr;		/* remote guys TO */
	uint32_t remote_len;		/* remote guys LEN */

	enum test_state state;		/* used for cond/signalling */
	sem_t evict_sem;
	sem_t sem;

        struct sockaddr_in sin;
        uint8_t addr_type;              /* ADDR_FAMILY - IPv4/V6 */
        uint8_t addr[16];                    /* dst addr in NBO */
        uint16_t port;                  /* dst port in NBO */
	int verbose;			/* verbose logging */

	/* CM stuff */
	pthread_t cmthread;
	struct rdma_event_channel *cm_channel;
	struct rdma_cm_id *cm_id;	/* connection on client side,*/
					/* listener on service side. */
	struct rdma_cm_id *child_cm_id;	/* connection on server side */

        struct rdma_session *sess;
        int rb_idx_map[MAX_MR_SIZE_GB];
        
        struct migration_remote_block_handler migrbh;
};

struct context {
  struct ibv_context *ctx;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  struct ibv_comp_channel *comp_channel;

  pthread_t recv_cq_thd;
};

struct rdma_mr_handler{
  char* region_list[MAX_FREE_MEM_GB];
  struct ibv_mr* mr_list[MAX_FREE_MEM_GB];
  int size_gb;
  int mapped_size;
  int cb_index_map[MAX_FREE_MEM_GB]; //rb is used by which connection, or -1
  int client_rb_idx[MAX_FREE_MEM_GB]; //rb is used by which connection, or -1
  int malloc_map[MAX_FREE_MEM_GB];
  int replica_index[MAX_FREE_MEM_GB];
};

struct rdma_session {
  int my_index;
  struct rping_cb *cb_list[MAX_CLIENT]; // need to init NULL
  enum cb_state cb_state_list[MAX_CLIENT];

  struct rping_cb **mig_cb_list; // need to init NULL
  enum cb_state *mig_cb_state_list;
  int mig_server_num;

  struct rdma_mr_handler mrh;
  struct portal *portal_list;
};

void reply_query(void *context);
void setup_connection(struct rdma_cm_id *cm_id, struct rping_cb *listening_cb, int mig_cb_index);
void request_migration(int local_rb_index, int src_rb_index, int src_cb_index);
int rping_create_connection(struct rping_cb *cb);
void fill_sockaddr(struct rping_cb *cb, struct portal *addr_info);
void* do_migration_fn(void *data);
#endif

