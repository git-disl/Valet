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

#ifndef VALET_DRV_H
#define VALET_DRV_H

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 37)
#include <asm/atomic.h>
#else
#include <linux/atomic.h>
#endif
#include <linux/completion.h>
#include <linux/list.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/fs.h>
#include <linux/wait.h>
#include <linux/fcntl.h>
#include <linux/cpumask.h>
#include <linux/configfs.h>
#include <linux/delay.h>

#include <linux/moduleparam.h>

extern struct list_head g_valet_sessions;
extern struct mutex g_lock;
extern int submit_queues; // num of available cpu (also connections)

// from kernel 
/*  host to network long long
 *  endian dependent
 *  http://www.bruceblinn.com/linuxinfo/ByteOrder.html
 */
#define ntohll(x) (((uint64_t)(ntohl((int)((x << 32) >> 32))) << 32) | \
		    (unsigned int)ntohl(((int)(x >> 32))))
#define htonll(x) ntohll(x)

#define htonll2(x) cpu_to_be64((x))
#define ntohll2(x) cpu_to_be64((x))
 
// comment below to disable "always-disk-backup"
//#define DISKBACKUP

#define MAX_SECTORS 8	// 32KB
//#define MAX_SECTORS 16	// 64KB
//#define MAX_SECTORS 32	// 128KB
//#define MAX_SECTORS 64	// 256KB
//#define MAX_SECTORS 128	// 512KB

#define RDMA_WR_BUF_LEN 128 // 512KB
//#define RDMA_WR_BUF_LEN 64 // 256KB
//#define RDMA_WR_BUF_LEN 32 // 128KB

#define MAX_MSG_LEN	    512
#define MAX_valet_DEV_NAME   256
#define SUPPORTED_DISKS	    256
#define SUPPORTED_PORTALS   5

#define VALET_SECT_SIZE	    512
#define VALET_SECT_SHIFT	    ilog2(VALET_SECT_SIZE)
#define VALET_QUEUE_DEPTH    256
#define QUEUE_NUM_MASK	0x001f	//used in addr->(mapping)-> rdma_queue in valet_main.c

#define SWAPSPACE_SIZE_G	40
#define BACKUP_DISK	"/dev/sda4"

#define VALET_PAGE_SIZE 4096

#define uint64_from_ptr(p)    (uint64_t)(uintptr_t)(p)
#define ptr_from_uint64(p)    (void *)(unsigned long)(p)

enum valet_dev_state {
        DEVICE_INITIALIZING,
        DEVICE_OPENNING,
        DEVICE_RUNNING,
        DEVICE_OFFLINE
};

static int major_num = 0;
module_param(major_num, int, 0);
static int LOGICAL_BLOCK_SIZE = 512;
module_param(LOGICAL_BLOCK_SIZE, int, 0);

static DECLARE_WAIT_QUEUE_HEAD(req_event);

struct valet_queue {
	unsigned int		     queue_depth;
        struct valet_session           *valet_sess;
	struct valet_file	    *xdev; /* pointer to parent*/
};

struct r_stat64 {
    uint64_t     st_size;    /* total size, in bytes */
 };

struct valet_file {
	int			     fd;
	int			     major; /* major number from kernel */
	struct r_stat64		     stbuf; /* remote file stats*/
	char			     file_name[MAX_valet_DEV_NAME];
	struct list_head	     list; /* next node in list of struct valet_file */
	struct gendisk		    *disk;
	struct request_queue	    *queue; /* The device request queue */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 16, 0)
	struct blk_mq_tag_set	     tag_set;
#endif
	struct valet_queue	    *queues;
	unsigned int		     queue_depth;
	unsigned int		     nr_queues;
	int			     index; /* drive idx */
	char			     dev_name[MAX_valet_DEV_NAME];
        struct valet_session           **valet_sess;
	struct config_group	     dev_cg;
	spinlock_t		     state_lock;
	enum valet_dev_state	     state;	
};

int valet_register_block_device(struct valet_file *valet_file);
void valet_unregister_block_device(struct valet_file *valet_file);
int valet_setup_queues(struct valet_file *xdev);
void valet_destroy_queues(struct valet_file *xdev);
int valet_create_device(struct valet_session *valet_session,
                       const char *xdev_name, struct valet_file *valet_file);
void valet_destroy_device(struct valet_session *valet_session,
                         struct valet_file *valet_file);
void valet_destroy_session_devices(struct valet_session *valet_session);
int valet_create_configfs_files(void);
void valet_destroy_configfs_files(void);
struct valet_file *valet_file_find(struct valet_session *valet_session,
                                 const char *name);
struct valet_session *valet_session_find_by_portal(struct list_head *s_data_list,
                                                 const char *portal);

#endif 

