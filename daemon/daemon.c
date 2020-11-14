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

#include "daemon.h"
#include "debug.h"

struct rdma_session session;
long page_size;  

static struct context *s_ctx = NULL;

static int get_addr(char *dst, struct sockaddr_in *addr)
{
	struct addrinfo *res;
	int ret;

	ret = getaddrinfo(dst, NULL, NULL, &res);
	if (ret) {
		printf("getaddrinfo failed - invalid hostname or IP address\n");
		return ret;
	}

	if (res->ai_family != PF_INET) {
		ret = -1;
		goto out;
	}

	*addr = *(struct sockaddr_in *) res->ai_addr;
out:
	freeaddrinfo(res);
	return ret;
}

static void portal_parser(FILE *fptr)
{
  char line[256];
  int p_count=0, i=0, j=0;
  int port;

  fgets(line, sizeof(line), fptr);
  sscanf(line, "%d", &p_count);
  session.mig_server_num = p_count;
  printf("daemon[%s]: num_servers : %d \n",__func__,session.mig_server_num);

  session.portal_list = malloc(sizeof(struct portal) * p_count);

  while (fgets(line, sizeof(line), fptr)) {
      printf("%s", line);
      j = 0;
      while (*(line + j) != ':'){
          j++;
      }
      memcpy(session.portal_list[i].addr, line, j);
      session.portal_list[i].addr[j] = '\0';
      port = 0;
      sscanf(line+j+1, "%d", &port);
      session.portal_list[i].port = (uint16_t)port;
      printf("daemon[%s]: portal: %s, %d\n",__func__, session.portal_list[i].addr, session.portal_list[i].port);
      i++;
  }
}

static void read_portal_file(char *filepath)
{
   FILE *fptr;

   printf("reading portal file.. [%s]\n",filepath);
   if ((fptr = fopen(filepath,"r")) == NULL){
       printf("portal file open failed.");
       exit(1);
   }

   portal_parser(fptr);

   fclose(fptr);
}

uint64_t htonll(uint64_t value)
{
     int num = 42;
     if(*(char *)&num == 42)
          return ((uint64_t)htonl(value & 0xFFFFFFFF) << 32LL) | htonl(value >> 32);
     else
          return value;
}

void die(const char *reason)
{
  fprintf(stderr, "%s\n", reason);
  exit(EXIT_FAILURE);
}

void post_send(struct rping_cb *cb)
{
      int ret=0;
      struct ibv_send_wr *bad_wr;

      cb->sq_wr.wr_id = (uintptr_t)cb;

      ret = ibv_post_send(cb->qp, &cb->sq_wr, &bad_wr);
      if (ret) {
		fprintf(stderr, "post send error %d\n", ret);
		exit(-1);
	}
}

void post_receive(struct rping_cb *cb)
{
      int ret=0;
      struct ibv_recv_wr *bad_wr;

      cb->rq_wr.wr_id = (uintptr_t)cb;

      ret = ibv_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret) {
		fprintf(stderr, "ibv_post_recv failed: %d\n", ret);
		exit(-1);
	}
}

void post_read(struct rping_cb *cb, uint32_t rkey, uint64_t addr, int region_index)
{
  int ret;
  struct ibv_send_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;

  memset(&wr, 0, sizeof(wr));

  wr.wr_id = (uintptr_t)cb;
  wr.opcode = IBV_WR_RDMA_READ;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED;
  wr.wr.rdma.remote_addr = (uintptr_t)addr;
  wr.wr.rdma.rkey = rkey;

  sge.addr = (uintptr_t)session.mrh.region_list[region_index];
  sge.length = sizeof(struct remote_block_header);
  sge.lkey = session.mrh.mr_list[region_index]->lkey;

  ret = ibv_post_send(cb->qp, &wr, &bad_wr);
  if (ret) {
	fprintf(stderr, "post send error %d\n", ret);
  }
}

void post_write(struct rping_cb *cb, uint32_t rkey, uint64_t addr, int region_index)
{
  int ret;
  struct ibv_send_wr wr[2], *bad_wr = NULL;
  struct ibv_sge sge_fst;
  struct ibv_sge sge_snd;

  memset(&wr[0], 0, sizeof(wr[0]));
  memset(&wr[1], 0, sizeof(wr[1]));

  sge_fst.addr = (uintptr_t)session.mrh.region_list[region_index];
  sge_fst.length = sizeof(struct remote_block_header) + HALF_ONE_GB;
  sge_fst.lkey = session.mrh.mr_list[region_index]->lkey;

  sge_snd.addr = (uintptr_t)(session.mrh.region_list[region_index] + sizeof(struct remote_block_header) + HALF_ONE_GB);
  sge_snd.length = HALF_ONE_GB;
  sge_snd.lkey = session.mrh.mr_list[region_index]->lkey;

  wr[0].opcode = IBV_WR_RDMA_WRITE;
  wr[0].sg_list = &sge_fst;
  wr[0].num_sge = 1;
  wr[0].send_flags = 0;
  wr[0].wr.rdma.remote_addr = (uintptr_t)addr;
  wr[0].wr.rdma.rkey = rkey;
  wr[0].next = &wr[1];

  wr[1].wr_id = (uintptr_t)cb;
  wr[1].opcode = IBV_WR_RDMA_WRITE;
  wr[1].sg_list = &sge_snd;
  wr[1].num_sge = 1;
  wr[1].send_flags = IBV_SEND_SIGNALED;
  wr[1].wr.rdma.remote_addr = (uintptr_t)(addr + sizeof(struct remote_block_header) + HALF_ONE_GB);
  wr[1].wr.rdma.rkey = rkey;
  wr[1].next = NULL;

  ret = ibv_post_send(cb->qp, &wr[0], &bad_wr);
  if (ret) {
	fprintf(stderr, "post send error %d\n", ret);
  }
}

long get_free_mem(void)
{
  char result[60];
  FILE *fd = fopen("/proc/meminfo", "r");
  int i;
  long res = 0;
  fgets(result, 60, fd);
  memset(result, 0x00, 60);
  fgets(result, 60, fd);
  for (i=0;i<60;i++){
    if (result[i] >= 48 && result[i] <= 57){
      res *= 10;
      res += (int)(result[i] - 48);
    }
  }
  fclose(fd);
  return res;
}

void rdma_session_init(struct rdma_session *sess, struct rping_cb *servercb){
  int free_mem_g;
  int i;
  page_size = getpagesize();

  free_mem_g = (int)(get_free_mem() / ONE_MB);
  printf("Valet[%s]: get free_mem %d\n", __func__, free_mem_g);
  for (i=0; i<MAX_FREE_MEM_GB; i++) {
    sess->mrh.cb_index_map[i] = -1;
    sess->mrh.malloc_map[i] = RB_EMPTY;
  }

  if (free_mem_g > FREE_MEM_EXPAND_THRESHOLD){
    free_mem_g -= (FREE_MEM_EVICT_THRESHOLD + FREE_MEM_EXPAND_THRESHOLD) / 2;
  } else if (free_mem_g > FREE_MEM_EVICT_THRESHOLD){
    free_mem_g  -= FREE_MEM_EVICT_THRESHOLD;
  }else{
    free_mem_g = 0;
  }
  if (free_mem_g > MAX_FREE_MEM_GB) {
    free_mem_g = MAX_FREE_MEM_GB;
  }

  for (i=0; i < free_mem_g; i++){
    posix_memalign((void **)&(sess->mrh.region_list[i]), page_size, sizeof(struct remote_block_header) + ONE_GB);
    memset(sess->mrh.region_list[i], 0x00, sizeof(struct remote_block_header) + ONE_GB);
    sess->mrh.malloc_map[i] = RB_MALLOCED;
  }
  sess->mrh.size_gb = free_mem_g;
  sess->mrh.mapped_size = 0;

  for (i=0; i<MAX_CLIENT; i++){
    sess->cb_list[i] = NULL;
    sess->cb_state_list[i] = CB_IDLE;
  }

  // mig cb list init
  sess->mig_cb_list = (struct rping_cb **)malloc(sizeof(struct rping_cb *) * sess->mig_server_num);
  sess->mig_cb_state_list = (enum cb_state *)malloc(sizeof(enum cb_state) * sess->mig_server_num);

  for (i=0; i<session.mig_server_num; i++) {
    printf("Valet[%s]: init mig cb addr:%s port:%d \n", __func__, sess->portal_list[i].addr, sess->portal_list[i].port);
    sess->mig_cb_state_list[i] = CB_IDLE;
    sess->mig_cb_list[i] = malloc(sizeof(struct rping_cb));
    memset(sess->mig_cb_list[i], 0, sizeof(struct rping_cb));
    sess->mig_cb_list[i]->state = IDLE;
    sess->mig_cb_list[i]->port = htons(sess->portal_list[i].port);
    inet_pton(AF_INET6, sess->portal_list[i].addr, &sess->mig_cb_list[i]->addr);
    sess->mig_cb_list[i]->cb_index = i;
    sess->mig_cb_list[i]->addr_type = AF_INET6;
    sem_init(&sess->mig_cb_list[i]->sem, 0, 0);

    get_addr(sess->portal_list[i].addr, &sess->mig_cb_list[i]->sin);
    sess->mig_cb_list[i]->sin.sin_port = htons(sess->portal_list[i].port);

    if(servercb->sin.sin_addr.s_addr == sess->mig_cb_list[i]->sin.sin_addr.s_addr && servercb->port == sess->mig_cb_list[i]->sin.sin_port){
        printf("Valet[%s]: This node matches to index[%d] mig_cb addr:%s port:%d \n", __func__,i, sess->portal_list[i].addr, sess->portal_list[i].port);
        sess->my_index=i;
    }

  }//for

  printf("Valet[%s]: memory allocated %d\n", __func__, sess->mrh.size_gb);

}

static int rping_cma_event_handler(struct rdma_cm_event *event)
{
	int ret = 0;
	int *mig_cb_index;
	struct rdma_cm_id *cma_id = event->id;
	struct rping_cb *cb = cma_id->context; // listening_cb

	printf("cma_event type %s cma_id %p (%s)\n",
		  rdma_event_str(event->event), cma_id,
		  (cma_id == cb->cm_id) ? "parent" : "child");

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		cb->state = ADDR_RESOLVED;
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			cb->state = ERROR;
			fprintf(stderr, "rdma_resolve_route error %d\n", ret);
			sem_post(&cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		cb->state = ROUTE_RESOLVED;
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		cb->state = CONNECT_REQUEST;
		cb->child_cm_id = cma_id;
                mig_cb_index = (int*)event->param.conn.private_data;
                printf("daemon[%s]: RDMA_CM_EVENT_CONNECT_REQUEST cma_id:%p cm_id:%p child_cm_id:%p\n",__func__,cma_id, cb->cm_id, cb->child_cm_id);
                setup_connection(cma_id,cb,*mig_cb_index); //incomming cm_id
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
    	        cb->state = CONNECTED;
                printf("daemon[%s]: RDMA_CM_EVENT_ESTABLISHED cma_id:%p cm_id:%p child_cm_id:%p cb:%p\n",__func__,cma_id, cb->cm_id, cb->child_cm_id,cb);
                if(cb->server){
                    reply_query(cb);
		}
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
                printf("receive RDMA_CM_EVENT_ADDR_ERROR \n");
		sem_post(&cb->sem);
		break;
	case RDMA_CM_EVENT_ROUTE_ERROR:
                printf("receive RDMA_CM_EVENT_ROUTE_ERROR \n");
		sem_post(&cb->sem);
		break;
	case RDMA_CM_EVENT_CONNECT_ERROR:
                printf("receive RDMA_CM_EVENT_CONNECT_ERROR \n");
		sem_post(&cb->sem);
		break;
	case RDMA_CM_EVENT_UNREACHABLE:
                printf("receive RDMA_CM_EVENT_UNREACHABLE \n");
		sem_post(&cb->sem);
		break;
	case RDMA_CM_EVENT_REJECTED:
                printf("receive RDMA_CM_EVENT_REJECTED \n");
		sem_post(&cb->sem);
		ret = -1;
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		fprintf(stderr, "DISCONNECT EVENT...\n");
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		fprintf(stderr, "cma detected device removal!!!!\n");
		ret = -1;
		break;

	default:
		fprintf(stderr, "unhandled event: %s, ignoring\n",
			rdma_event_str(event->event));
		break;
	}

	return ret;
}

void reply_query(void *context)
{
  struct rping_cb *cb = (struct rping_cb *)context;
  struct timeval tmp_tv;
  unsigned long sum=0;
  int free_mem=0;
  int i;

  cb->qmsg.type = RESP_QUERY;
  free_mem = session.mrh.size_gb - session.mrh.mapped_size;
  if(free_mem < 0)
      cb->qmsg.size_gb = 0;
  else
      cb->qmsg.size_gb = free_mem;

  for (i = 0; i < MAX_FREE_MEM_GB; i++){
      if (session.mrh.cb_index_map[i] != -1){
            memcpy(&tmp_tv, session.mrh.region_list[i], sizeof(struct timeval));
            sum += tmp_tv.tv_sec*1000000L + tmp_tv.tv_usec;
            printf("TimeStamp : rb[%d] : %ld , sum : %lu \n", i, tmp_tv.tv_sec*1000000L + tmp_tv.tv_usec, sum);
      }
  }
  cb->qmsg.sum = sum;

  printf("Valet[%s] : send free mem : %d , sum : %lu\n", __func__, cb->qmsg.size_gb, cb->qmsg.sum);
  memcpy(cb->send_buf ,&cb->qmsg,sizeof(struct query_message));
  post_send(cb);
}

void send_evict(void *context, int evict_index, int client_rb_index, int src_cb_index)
{
  int i;
  struct rping_cb *cb = (struct rping_cb *)context;

  cb->send_buf->type = EVICT;
  cb->send_buf->size_gb = client_rb_index;
  cb->send_buf->cb_index = src_cb_index;
  cb->send_buf->rb_index = evict_index;

  for (i=0; i<MAX_MR_SIZE_GB; i++){
    if(i == evict_index){
        cb->send_buf->rkey[i] = htonl((uint64_t)session.mrh.mr_list[i]->rkey);
        cb->send_buf->buf[i] = htonll((uint64_t)session.mrh.mr_list[i]->addr);
        cb->send_buf->replica_index = session.mrh.replica_index[i];
    }else{
        cb->send_buf->rkey[i] = 0;
    }
  }

  //printf("Valet[%s] : send_evict rb[%d] rkey:%"PRIu32", addr:%"PRIu64", replica_index:%d \n", __func__,evict_index,cb->send_buf->rkey[evict_index],cb->send_buf->buf[evict_index],cb->send_buf->replica_index);
  post_send(cb);
}

int prepare_single_mr(void *context, int isMigration)
{
  struct rping_cb *cb = (struct rping_cb *)context;
  int i = 0;
  int client_rb_index = cb->recv_buf->size_gb;

  for (i=0; i<MAX_FREE_MEM_GB;i++){
    cb->send_buf->rkey[i] = 0;
  }
  for (i=0; i<MAX_FREE_MEM_GB; i++) {
    if (session.mrh.malloc_map[i] == RB_MALLOCED && session.mrh.cb_index_map[i] == -1) {
      cb->rb_idx_map[i] = i;
      session.mrh.replica_index[i] = cb->recv_buf->replica_index;
      session.mrh.cb_index_map[i] = cb->cb_index; // this cb_index is for local 
      session.mrh.client_rb_idx[i] = client_rb_index;
      session.mrh.mr_list[i] = ibv_reg_mr(s_ctx->pd, session.mrh.region_list[i], sizeof(struct remote_block_header) + ONE_GB, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
      cb->send_buf->buf[i] = htonll((uint64_t)session.mrh.mr_list[i]->addr);
      cb->send_buf->rkey[i] = htonl((uint64_t)session.mrh.mr_list[i]->rkey);
      break;
    }
  }

  session.mrh.mapped_size += 1;

  cb->send_buf->size_gb = client_rb_index;
  cb->send_buf->replica_index = cb->recv_buf->replica_index;
  if(isMigration){
      cb->send_buf->type = RESP_MIG_MAP;
      cb->send_buf->cb_index = session.my_index; // this is global my ID
      cb->send_buf->rb_index = i;
  }else{
      cb->send_buf->type = RESP_MR_MAP;
  }
  
  return i;
}

void* do_migration_fn(void *data)
{
     struct rping_cb *cb = data;
     struct rping_cb *mig_cb;

     while (1) {
         sem_wait(&cb->migrbh.migration_sem);
         request_migration(cb->migrbh.local_rb_index,cb->migrbh.src_rb_index,cb->migrbh.src_cb_index);
     }//while
}

void request_migration(int local_rb_index, int src_rb_index, int src_cb_index)
{
  int i;
  struct rping_cb *mig_cb;
  uint64_t addr;
  uint32_t rkey;

  mig_cb =  session.mig_cb_list[src_cb_index];

  // check if connected
  if(mig_cb->state < CONNECTED){
      rping_create_connection(mig_cb);
  }

  // send migration request message to src node
  mig_cb->send_buf->size_gb = src_rb_index;
  mig_cb->send_buf->type = REQ_MIGRATION;
  for (i=0; i<MAX_MR_SIZE_GB; i++){
    if(i==local_rb_index){
        mig_cb->send_buf->rkey[i] = htonl((uint64_t)session.mrh.mr_list[i]->rkey);
        mig_cb->send_buf->buf[i] = htonll((uint64_t)session.mrh.mr_list[i]->addr);
    }else{
        mig_cb->send_buf->rkey[i] = 0;
    }
  }
  post_send(mig_cb);

}

void do_migration(struct rping_cb *cb)
{
  int i;
  struct rping_cb *mig_cb;
  int dest_cb_index;
  uint64_t addr;
  uint32_t rkey;

  cb->migrbh.local_rb_index = cb->recv_buf->size_gb;
  dest_cb_index = cb->recv_buf->cb_index;

  for (i = 0 ; i<MAX_MR_SIZE_GB; i++) {
    if (cb->recv_buf->rkey[i]){
	 rkey = ntohl(cb->recv_buf->rkey[i]);
	 addr = ntohll(cb->recv_buf->buf[i]);
	 break;
    }
  }
  cb->state = RDMA_WRITE_INP;
  post_write(cb, rkey, addr, cb->migrbh.local_rb_index);
}

void recv_delete_rb(struct rping_cb *cb)
{
  int i, index;

  for (i = 0 ; i<MAX_MR_SIZE_GB; i++) {
    if (cb->recv_buf->rkey[i]){ //stopped this one
      index = cb->rb_idx_map[i];
      cb->rb_idx_map[i] = -1;
      ibv_dereg_mr(session.mrh.mr_list[index]);
      free(session.mrh.region_list[index]);
      session.mrh.cb_index_map[index] = -1;
      session.mrh.malloc_map[index] = RB_EMPTY;
      break;
    }
  }

  session.mrh.size_gb -= 1;
  session.mrh.mapped_size -= 1;

  sem_post(&cb->evict_sem);
}

static int handle_recv(struct rping_cb *cb)
{
     int i;
     struct query_message qmsg;

     switch (cb->recv_buf->type){
      case REQ_QUERY:
        reply_query(cb);
        break;
      case REQ_MIG_MAP:
        cb->state = RECV_MR_MAP;

        session.cb_state_list[cb->cb_index] = CB_MAPPED;

        cb->migrbh.client_rb_index = cb->recv_buf->size_gb;
        cb->migrbh.src_rb_index = cb->recv_buf->rb_index;
        cb->migrbh.src_cb_index = cb->recv_buf->cb_index;
        for (i = 0 ; i<MAX_MR_SIZE_GB; i++) {
          if (cb->recv_buf->rkey[i]){
	    cb->migrbh.rkey[i] = ntohl(cb->recv_buf->rkey[i]);
	    cb->migrbh.buf[i] = ntohll(cb->recv_buf->buf[i]);
	    break;
          }
        }
        cb->migrbh.local_rb_index = prepare_single_mr(cb, 1);
        post_send(cb);
        break;
      case REQ_MR_MAP:
        prepare_single_mr(cb, 0);
        post_send(cb);
        session.cb_state_list[cb->cb_index] = CB_MAPPED;
        break;
      case DELETE_RB:
        recv_delete_rb(cb);
        break;
      case RESP_QUERY:
	memcpy(&qmsg,&cb->recv_buf,sizeof(qmsg));
	cb->state = RECV_QUERY_REPL;
        break;
      case REQ_MIGRATION:
	cb->state = RECV_MIG_MAP;
        do_migration(cb);
        break;
      default:
        printf("unknown received message : %d\n",cb->recv_buf->type);
        return -1;
    }

	return 0;
}

void do_evict(int evict_size)
{
  int i;
  int oldest_rb_index=-1;
  int freed_g = 0;
  int client_cb_index;
  struct rping_cb *client_cb;
  struct timeval oldest;
  struct timeval tmp_tv;

  //free unmapped rb
  for (i = 0; i < MAX_FREE_MEM_GB ;i++) {
    if (session.mrh.malloc_map[i] == RB_MALLOCED && session.mrh.cb_index_map[i] == -1){
      free(session.mrh.region_list[i]);
      session.mrh.malloc_map[i] = RB_EMPTY;
      freed_g += 1;
      if (freed_g == evict_size){
        session.mrh.size_gb -= evict_size;
        return;
      }
    }
  }
  //not enough
  session.mrh.size_gb -= freed_g;
  evict_size -= freed_g;

  if (session.mrh.mapped_size < evict_size){
      evict_size = session.mrh.mapped_size;
  }
  while(evict_size > 0){
  // sort by age
  // pick the oldest
  oldest.tv_sec=0;
  oldest.tv_usec=0;
  for (i = 0; i < MAX_FREE_MEM_GB; i++){
      if (session.mrh.cb_index_map[i] != -1){
            memcpy(&tmp_tv, session.mrh.region_list[i], sizeof(struct timeval));
            if(oldest.tv_sec==0 && oldest.tv_usec==0){
               oldest = tmp_tv;
               oldest_rb_index = i;
            }else{
               if(oldest.tv_sec*1000000L + oldest.tv_usec > tmp_tv.tv_sec*1000000L + tmp_tv.tv_usec){
                   oldest = tmp_tv;
                   oldest_rb_index = i;
               }
            }
            printf("TimeStamp : rb[%d] : %ld , oldest : %ld \n",i,tmp_tv.tv_sec*1000000L + tmp_tv.tv_usec,oldest.tv_sec*1000000L + oldest.tv_usec);
      }
  }

  // send evict message of the oldest rb to client
  client_cb_index = session.mrh.cb_index_map[oldest_rb_index];
  client_cb = session.cb_list[client_cb_index];
  send_evict(client_cb, oldest_rb_index, session.mrh.client_rb_idx[oldest_rb_index], session.my_index );

  sem_wait(&client_cb->evict_sem);
  --evict_size;
  }//while
}

static int rping_cq_event_handler(struct rping_cb *cb, struct ibv_wc *wc)
{
	int ret = 0;
        int client_cb_index;
        struct rping_cb *client_cb;

	if (wc->status) {
		fprintf(stderr, "cq completion failed status %s\n",
		ibv_wc_status_str(wc->status));
		if (wc->status != IBV_WC_WR_FLUSH_ERR)
			ret = -1;
		goto error;
	}

	switch (wc->opcode) {
	case IBV_WC_SEND:
       		if(cb->state == RECV_MR_MAP){
       		    sem_post(&cb->migrbh.migration_sem);
		}
		cb->state = RDMA_SEND_COMPLETE;
		break;
	case IBV_WC_RDMA_WRITE:
		// send migration done message to client
  		client_cb_index = session.mrh.cb_index_map[cb->migrbh.local_rb_index];
  		client_cb = session.cb_list[client_cb_index];
  		client_cb->send_buf->type = DONE_MIGRATION;
  		post_send(client_cb);

		cb->state = RDMA_WRITE_COMPLETE;
		break;
	case IBV_WC_RDMA_READ:
		cb->state = RDMA_READ_COMPLETE;
  		sem_post(&cb->migrbh.migration_sem);
		break;
	case IBV_WC_RECV:
		handle_recv(cb);
		post_receive(cb);
		break;
	default:
		DEBUG_LOG("unknown work completion\n");
		ret = -1;
		goto error;
	}

	return 0;

error:
	cb->state = ERROR;
	return ret;
}

static int rping_accept(struct rping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	DEBUG_LOG("accepting client connection request\n");

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
        conn_param.rnr_retry_count = 7;

	ret = rdma_accept(cb->cm_id, &conn_param);
	if (ret) {
		fprintf(stderr, "rdma_accept error: %d\n", ret);
		return ret;
	}

	return 0;
}

static void rping_setup_wr(struct rping_cb *cb)
{
	cb->recv_sgl.addr = (uintptr_t) cb->recv_buf;
	cb->recv_sgl.length = sizeof(struct message);
	cb->recv_sgl.lkey = cb->recv_mr->lkey;
	cb->rq_wr.sg_list = &cb->recv_sgl;
	cb->rq_wr.num_sge = 1;

	cb->send_sgl.addr = (uintptr_t) cb->send_buf;
	cb->send_sgl.length = sizeof(struct message);
	cb->send_sgl.lkey = cb->send_mr->lkey;
	cb->sq_wr.opcode = IBV_WR_SEND;
	cb->sq_wr.send_flags = IBV_SEND_SIGNALED;
	cb->sq_wr.sg_list = &cb->send_sgl;
	cb->sq_wr.num_sge = 1;
}

static int rping_setup_buffers(struct rping_cb *cb)
{
	int ret;

        cb->send_buf = malloc(sizeof(struct message));
        cb->recv_buf = malloc(sizeof(struct message));

        DEBUG_LOG("rping_setup_buffers called on cb %p\n", cb);
        cb->recv_mr = ibv_reg_mr(s_ctx->pd, cb->recv_buf, sizeof(struct message),
				 IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
	if (!cb->recv_mr) {
	    fprintf(stderr, "recv_buf reg_mr failed\n");
	    return errno;
        }

        cb->send_mr = ibv_reg_mr(s_ctx->pd, cb->send_buf, sizeof(struct message),
                                 IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
        if (!cb->send_mr) {
		fprintf(stderr, "send_buf reg_mr failed\n");
		ret = errno;
		goto err1;
        }

	rping_setup_wr(cb);
	DEBUG_LOG("allocated & registered buffers...\n");
	return 0;
err1:
	ibv_dereg_mr(cb->recv_mr);
	return ret;
}

static void rping_free_buffers(struct rping_cb *cb)
{
	DEBUG_LOG("rping_free_buffers called on cb %p\n", cb);
	ibv_dereg_mr(cb->recv_mr);
	ibv_dereg_mr(cb->send_mr);
}

static int rping_create_qp(struct rping_cb *cb)
{
	struct ibv_qp_init_attr init_attr;
	int ret=0;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = RPING_SQ_DEPTH;
	init_attr.cap.max_recv_wr = RPING_RQ_DEPTH;
	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IBV_QPT_RC;

	if (cb->server) {
	        init_attr.send_cq = s_ctx->cq;
	        init_attr.recv_cq = s_ctx->cq;
		ret = rdma_create_qp(cb->cm_id, s_ctx->pd, &init_attr);
	} else {
	        init_attr.send_cq = cb->cq;
		init_attr.recv_cq = cb->cq;
		ret = rdma_create_qp(cb->cm_id, s_ctx->pd, &init_attr);
	}

	if (!ret){
		cb->qp = cb->cm_id->qp;
	}else{
                printf("fail to create qp %d\n",ret);
        }

	return ret;
}

static void rping_free_qp(struct rping_cb *cb)
{
        ibv_destroy_qp(cb->qp);
        if(cb->server){
	    ibv_destroy_cq(s_ctx->cq);
	    ibv_destroy_comp_channel(s_ctx->comp_channel);
	    ibv_dealloc_pd(s_ctx->pd);
        }else{
	    ibv_destroy_cq(cb->cq);
	    ibv_destroy_comp_channel(cb->channel);
	    ibv_dealloc_pd(cb->pd);
        }
}

static void * receiver_cq_thread(void *ctx)
{
  struct ibv_cq *cq;
  struct ibv_wc wc;
  int curr;
  int ret;
  int ne;

  while (1) {

        if(ibv_get_cq_event(s_ctx->comp_channel, &cq, &ctx) != 0){
            printf("ibv_get_cq_event fail\n");
            exit(-1);
        }
        ibv_ack_cq_events(cq, 1);
        if(ibv_req_notify_cq(cq, 0) != 0 ){
            printf("ibv_req_notify_cq fail\n");
            exit(-1);
        }

        while (ibv_poll_cq(cq, 1, &wc))
            rping_cq_event_handler((struct rping_cb *)(uintptr_t)wc.wr_id, &wc);
   }

  return NULL;
}

static void *sender_cq_thread(void *arg)
{
	struct rping_cb *cb = arg;
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	int ret;
        struct ibv_wc wc;
        int curr;
        int ne;
	
	DEBUG_LOG("cq_thread started.\n");

	while (1) {	
		pthread_testcancel();
                ret = ibv_get_cq_event(cb->channel, &ev_cq, &ev_ctx);
                if (ret) {
                        fprintf(stderr, "Failed to get cq event!\n");
                        pthread_exit(NULL);
                }
                if (ev_cq != cb->cq) {
                        fprintf(stderr, "Unknown CQ!\n");
                        pthread_exit(NULL);
                }
                ret = ibv_req_notify_cq(cb->cq, 0);
                if (ret) {
                        fprintf(stderr, "Failed to set notify!\n");
                        pthread_exit(NULL);
                }

	        while ((ret = ibv_poll_cq(cb->cq, 1, &wc)) == 1) {
                    ret = rping_cq_event_handler(cb, &wc);
                }
                    ibv_ack_cq_events(cb->cq, 1);
                    if (ret)
                        pthread_exit(NULL);
	}//while
}

static int rping_setup_qp(struct rping_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;
  	cb->pd = s_ctx->pd;

	cb->channel = ibv_create_comp_channel(cm_id->verbs);
	if (!cb->channel) {
		fprintf(stderr, "ibv_create_comp_channel failed\n");
		ret = errno;
		goto err1;
	}
	DEBUG_LOG("created channel %p\n", cb->channel);

	cb->cq = ibv_create_cq(cm_id->verbs, 100, cb, cb->channel, 0);
	if (!cb->cq) {
		fprintf(stderr, "ibv_create_cq failed\n");
		ret = errno;
		goto err2;
	}
	DEBUG_LOG("created cq %p\n", cb->cq);

	ret = ibv_req_notify_cq(cb->cq, 0);
	if (ret) {
		fprintf(stderr, "ibv_req_notify_cq failed\n");
		ret = errno;
		goto err3;
	}

	ret = rping_create_qp(cb);
	if (ret) {
		fprintf(stderr, "rping_create_qp failed: %d\n", ret);
		goto err3;
	}
	DEBUG_LOG("created qp %p\n", cb->qp);
	return 0;

err3:
	ibv_destroy_cq(cb->cq);
err2:
	ibv_destroy_comp_channel(cb->channel);
err1:
	ibv_dealloc_pd(cb->pd);
	return ret;
}

static int rping_server_setup_qp(struct rdma_cm_id *cm_id, struct rping_cb *listening_cb)
{
	int ret;

        if (s_ctx) {
            printf("s_ctx exist\n");

            if (s_ctx->ctx != cm_id->verbs)
                die("cannot handle events in more than one context.\n");

            goto initdone;
        }

        s_ctx = (struct context *)malloc(sizeof(struct context));

        s_ctx->ctx = cm_id->verbs;

  	s_ctx->pd = ibv_alloc_pd(s_ctx->ctx);
	if (!s_ctx->pd) {
		fprintf(stderr, "ibv_alloc_pd failed\n");
		return errno;
	}
	DEBUG_LOG("created pd %p\n", s_ctx->pd);

	s_ctx->comp_channel = ibv_create_comp_channel(s_ctx->ctx);
	if (!s_ctx->comp_channel) {
		fprintf(stderr, "ibv_create_comp_channel failed\n");
		ret = errno;
		goto err1;
	}
	DEBUG_LOG("created channel %p\n", s_ctx->comp_channel);

	s_ctx->cq = ibv_create_cq(s_ctx->ctx, 100, listening_cb, s_ctx->comp_channel, 0);
	if (!s_ctx->cq) {
		fprintf(stderr, "ibv_create_cq failed\n");
		ret = errno;
		goto err2;
	}
	DEBUG_LOG("created cq %p\n", s_ctx->cq);

	ret = ibv_req_notify_cq(s_ctx->cq, 0);
	if (ret) {
		fprintf(stderr, "ibv_req_notify_cq failed\n");
		ret = errno;
		goto err3;
	}

        pthread_create(&s_ctx->recv_cq_thd, NULL, receiver_cq_thread, NULL);

initdone:

	return 0;

err3:
	ibv_destroy_cq(s_ctx->cq);
err2:
	ibv_destroy_comp_channel(s_ctx->comp_channel);
err1:
	ibv_dealloc_pd(s_ctx->pd);

	return ret;
}

static void *cm_thread(void *arg)
{
	struct rping_cb *cb = arg;
	struct rdma_cm_event *event;
	int ret;

	while (1) {
		ret = rdma_get_cm_event(cb->cm_channel, &event);
		if (ret) {
			fprintf(stderr, "rdma_get_cm_event err %d\n", ret);
			exit(ret);
		}
                struct rdma_cm_event event_copy;

   		memcpy(&event_copy, event, sizeof(*event));
		rdma_ack_cm_event(event);

		ret = rping_cma_event_handler(&event_copy);
		if (ret)
			exit(ret);
	}
}

void fill_sockaddr(struct rping_cb *cb, struct portal *addr_info)
{
  if (cb->addr_type == AF_INET) {
    cb->port = htons(addr_info->port);
    inet_pton(AF_INET, addr_info->addr, &cb->addr);

    get_addr(addr_info->addr, &cb->sin);
    cb->sin.sin_port = htons(addr_info->port);

  } else if (cb->addr_type == AF_INET6) {
    cb->port = htons(addr_info->port);
    inet_pton(AF_INET6, addr_info->addr, &cb->addr);

    get_addr(addr_info->addr, &cb->sin);
    cb->sin.sin_port = htons(addr_info->port);
  }
}

static int rping_bind_server(struct rping_cb *cb)
{
	int ret;
        uint16_t port = 0;
        char buffer[20];

        cb->sin.sin_port = cb->port;
      	ret = rdma_bind_addr(cb->cm_id,(struct sockaddr *)&cb->sin);
	if (ret) {
		fprintf(stderr, "rdma_bind_addr error %d\n", ret);
		return ret;
	}

	ret = rdma_listen(cb->cm_id, 3);
	if (ret) {
		fprintf(stderr, "rdma_listen failed: %d\n", ret);
		return ret;
	}
        port = ntohs(rdma_get_src_port(cb->cm_id));

        inet_ntop(AF_INET6, rdma_get_local_addr(cb->cm_id), buffer, 20);
        printf("%s : listening on port %s %d.\n",__func__,buffer, port);

	return 0;
}

static struct rping_cb *clone_cb(struct rping_cb *listening_cb)
{
	struct rping_cb *cb = malloc(sizeof(struct rping_cb));
	if (!cb)
		return NULL;

	cb->cm_id = listening_cb->child_cm_id;
	cb->cm_id->context = cb;
	cb->child_cm_id = listening_cb->cm_id;
	cb->child_cm_id->context = listening_cb;

	return cb;
}

static void free_cb(struct rping_cb *cb)
{
	free(cb);
}

void setup_connection(struct rdma_cm_id *cm_id, struct rping_cb *listening_cb, int mig_cb_index)
{
	struct ibv_recv_wr *bad_wr;
	int ret;
        int i;

	struct rping_cb *cb = malloc(sizeof(struct rping_cb));
	if (!cb){
	    DEBUG_LOG("fail to allocate cb \n");
	    return;
        }

        cb->server = 1;
	cb->cm_id = cm_id;
        cb->cm_id->context = cb;

        ret = rping_server_setup_qp(cm_id,listening_cb);
	if (ret) {
		fprintf(stderr, "setup_qp failed: %d\n", ret);
		goto err0;
	}

	ret = rping_create_qp(cb);
	if (ret) {
		fprintf(stderr, "rping_create_qp failed: %d\n", ret);
		goto err3;
	}
	DEBUG_LOG("created qp %p\n", cb->qp);

        cb->sess = &session;
        for (i = 0; i < MAX_FREE_MEM_GB; i++){
            cb->rb_idx_map[i] = -1;
        }

        sem_init(&cb->evict_sem, 0, 0);
        sem_init(&cb->migrbh.migration_sem, 0, 0);

        if(mig_cb_index != -1){
	    DEBUG_LOG("setup mig_cb[%d] \n",mig_cb_index);
            //add to session 
            if (session.mig_cb_state_list[mig_cb_index] == CB_IDLE){
	        DEBUG_LOG("add mig_cb to the list : %d \n",mig_cb_index);
                session.mig_cb_list[mig_cb_index] = cb;
                session.mig_cb_state_list[mig_cb_index] = CB_CONNECTED;
                cb->cb_index = mig_cb_index;
            }else{
                // not going to happen
	        DEBUG_LOG("already addded to the list \n");
	    }
	}else{
	    DEBUG_LOG("setup client_cb[%d] \n",mig_cb_index);

            pthread_create(&cb->migrbh.migthread, NULL, do_migration_fn, cb);

            //add to session 
            for (i=0; i<MAX_CLIENT; i++){
                if (session.cb_state_list[i] == CB_IDLE){
                session.cb_list[i] = cb;
                session.cb_state_list[i] = CB_CONNECTED;
                cb->cb_index = i;
                break;
                }
            }
 	}//else

	ret = rping_setup_buffers(cb);
	if (ret) {
		fprintf(stderr, "rping_setup_buffers failed: %d\n", ret);
		goto err1;
	}

        post_receive(cb);

	ret = rping_accept(cb);
	if (ret) {
		fprintf(stderr, "connect error %d\n", ret);
		goto err3;
	}

	return ;
err3:
	pthread_cancel(s_ctx->recv_cq_thd);
	pthread_join(s_ctx->recv_cq_thd, NULL);
err2:
	rping_free_buffers(cb);
err1:
	rping_free_qp(cb);
err0:
	free_cb(cb);
	return ;
}

static int start_persistent_listener(struct rping_cb *listening_cb)
{
        int ret;
	struct rping_cb *cb;
	struct rdma_cm_event *event;

        listening_cb->server = 1;

	listening_cb->cm_channel = rdma_create_event_channel();
	if (!listening_cb->cm_channel) {
		ret = errno;
		fprintf(stderr, "rdma_create_event_channel error %d\n", ret);
		return ret;
	}

	ret = rdma_create_id(listening_cb->cm_channel, &listening_cb->cm_id, listening_cb, RDMA_PS_TCP);
	if (ret) {
		ret = errno;
		fprintf(stderr, "rdma_create_id error %d\n", ret);
		return ret;
	}
	DEBUG_LOG("created cm_id %p\n", listening_cb->cm_id);

	ret = rping_bind_server(listening_cb);
	if (ret)
		return ret;
 
         while (1) {
                 ret = rdma_get_cm_event(listening_cb->cm_channel, &event);
                 if (ret) {
                         fprintf(stderr, "rdma_get_cm_event err %d\n", ret);
                         exit(ret);
                 }
                 struct rdma_cm_event event_copy;
 
                 memcpy(&event_copy, event, sizeof(*event));
                 rdma_ack_cm_event(event);
 
                 ret = rping_cma_event_handler(&event_copy);
                 if (ret)
                         exit(ret);
        }

        DEBUG_LOG("destroy cm_id %p\n", listening_cb->cm_id);
        rdma_destroy_id(listening_cb->cm_id);
        rdma_destroy_event_channel(listening_cb->cm_channel);
        free(listening_cb);

    return 0;
}

static int rping_connect_client(struct rping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;
        int mig_cb_index = session.my_index;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 10;
        conn_param.private_data = (int*)&mig_cb_index; // mark for mig_cb connection
        conn_param.private_data_len = sizeof(int);

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		fprintf(stderr, "rdma_connect error %d\n", ret);
		return ret;
	}

        DEBUG_LINE
	sem_wait(&cb->sem);
        DEBUG_LINE
	if (cb->state != CONNECTED) {
		fprintf(stderr, "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}

	DEBUG_LOG("rmda_connect successful\n");
	return 0;
}

static int rping_bind_client(struct rping_cb *cb)
{
	int ret;

	ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *) &cb->sin, 2000);
	if (ret) {
		fprintf(stderr, "rdma_resolve_addr error %d\n", ret);
		return ret;
	}

	sem_wait(&cb->sem);
	if (cb->state != ROUTE_RESOLVED) {
		fprintf(stderr, "waiting for addr/route resolution state %d\n",
			cb->state);
		return -1;
	}

	DEBUG_LOG("rdma_resolve_addr - rdma_resolve_route successful\n");
	return 0;
}

int rping_create_connection(struct rping_cb *cb)
{
	struct ibv_recv_wr *bad_wr;
	int ret;

	cb->cm_channel = rdma_create_event_channel();
	if (!cb->cm_channel) {
		ret = errno;
		fprintf(stderr, "rdma_create_event_channel error %d\n", ret);
		return ret;
	}

	ret = rdma_create_id(cb->cm_channel, &cb->cm_id, cb, RDMA_PS_TCP);
	if (ret) {
		ret = errno;
		fprintf(stderr, "rdma_create_id error %d\n", ret);
		return ret;
	}
	DEBUG_LOG("created cm_id %p\n", cb->cm_id);

	pthread_create(&cb->cmthread, NULL, cm_thread, cb);

	ret = rping_bind_client(cb);
	if (ret)
		return ret;

	ret = rping_setup_qp(cb, cb->cm_id);
	if (ret) {
		fprintf(stderr, "setup_qp failed: %d\n", ret);
		return ret;
	}

	ret = rping_setup_buffers(cb);
	if (ret) {
		fprintf(stderr, "rping_setup_buffers failed: %d\n", ret);
		goto err1;
	}

        post_receive(cb);      
	pthread_create(&cb->send_cq_thd, NULL, sender_cq_thread, cb);

	ret = rping_connect_client(cb);
	if (ret) {
		fprintf(stderr, "connect error %d\n", ret);
		goto err2;
	}
 
        return ret;

err2:
	rping_free_buffers(cb);
err1:
	rping_free_qp(cb);

	return ret;
}

void* free_mem_monitor_fn(void *data)
{
  int free_mem_g = 0;
  int last_free_mem_g;
  int filtered_free_mem_g = 0;
  int evict_hit_count = 0;
  int expand_hit_count = 0;
  float last_free_mem_weight = 1 - CURR_FREE_MEM_WEIGHT;
  int stop_size_g;
  int expand_size_g;
  int i, j;
  struct timeval tv;
  struct rping_cb *mig_cb;
  int running=1;

  last_free_mem_g = (int)(get_free_mem() / ONE_MB);

  while (running) {
    free_mem_g = (int)(get_free_mem() / ONE_MB);
    filtered_free_mem_g = (int)(CURR_FREE_MEM_WEIGHT * free_mem_g + last_free_mem_g * last_free_mem_weight);
    last_free_mem_g = filtered_free_mem_g;

    if (filtered_free_mem_g < FREE_MEM_EVICT_THRESHOLD){
      evict_hit_count += 1;
      expand_hit_count = 0;
      if (evict_hit_count >= MEM_EVICT_HIT_THRESHOLD){
        evict_hit_count = 0;
        //evict  down_threshold - free_mem
        stop_size_g = FREE_MEM_EVICT_THRESHOLD - last_free_mem_g;
        if (session.mrh.size_gb < stop_size_g){
          stop_size_g = session.mrh.size_gb;
        }
        if (stop_size_g > 0){ //stop_size_g has to be meaningful.
          do_evict(stop_size_g);
        }
        last_free_mem_g += stop_size_g;
      }
    }else if (filtered_free_mem_g > FREE_MEM_EXPAND_THRESHOLD) {
      expand_hit_count += 1;
      evict_hit_count = 0;
      if (expand_hit_count >= MEM_EXPAND_HIT_THRESHOLD){
        expand_hit_count = 0;
        expand_size_g =  last_free_mem_g - FREE_MEM_EXPAND_THRESHOLD;
        if ((expand_size_g + session.mrh.size_gb) > MAX_FREE_MEM_GB) {
          expand_size_g = MAX_FREE_MEM_GB - session.mrh.size_gb;
        }
        j = 0;
        for (i = 0; i < MAX_FREE_MEM_GB; i++){
          if (session.mrh.malloc_map[i] == RB_EMPTY){
            posix_memalign((void **)&(session.mrh.region_list[i]), page_size, sizeof(struct remote_block_header) + ONE_GB);
            memset(session.mrh.region_list[i], 0x00, sizeof(struct remote_block_header) + ONE_GB);
            session.mrh.malloc_map[i] = RB_MALLOCED;
            j += 1;
            if (j == expand_size_g){
              break;
            }
          }
        }
        session.mrh.size_gb += expand_size_g;
        last_free_mem_g -= expand_size_g;
      }
    }

    sleep(1);
  }
  return NULL;
}

static void usage(char *name)
{
	printf("%s -a [ipaddr] -p [port] -i [portal_file]\n",name);
        exit(1);
}

int main(int argc, char *argv[])
{
	struct rping_cb *cb;
	int ret = 0;

        pthread_t client_session_thread;
        pthread_t free_mem_monitor_thread;
  
	page_size = sysconf(_SC_PAGE_SIZE);
 	
	cb = malloc(sizeof(*cb));

	DEBUG_LOG("create listening_cb %p\n", cb);

	if (!cb)
		return -ENOMEM;

	memset(cb, 0, sizeof(*cb));
	cb->state = IDLE;
	sem_init(&cb->sem, 0, 0);

        int op;
	int opterr = 0;
	while ((op=getopt(argc, argv, "a:p:i:")) != -1) {
		switch (op) {
		case 'a':
			ret = get_addr(optarg, &cb->sin);
			break;
		case 'p':
			cb->port = htons(atoi(optarg));
			DEBUG_LOG("port %d\n", (int) atoi(optarg));
			break;
                case 'i':
                        printf ("Input file: \"%s\"\n", optarg);
                        read_portal_file(optarg);
                        break;
		default:
			ret = EINVAL;
			goto out;
		}
	}
	if (ret)
		goto out;

        rdma_session_init(&session, cb);

        pthread_create(&free_mem_monitor_thread, NULL, free_mem_monitor_fn, NULL);

        start_persistent_listener(cb);

	DEBUG_LOG("destroy cm_id %p\n", cb->cm_id);
	rdma_destroy_id(cb->cm_id);
out2:
	rdma_destroy_event_channel(cb->cm_channel);
out:
	free(cb);
	return ret;
}
