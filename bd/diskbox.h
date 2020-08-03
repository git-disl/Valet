/*
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

#ifndef DISKBOX_H
#define DISKBOX_H

#include <linux/errno.h>  /* error codes */
#include <linux/types.h>  /* size_t */
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/hdreg.h>
#include <trace/events/block.h>

#define BACKUP_DISK	"/dev/sda4"
#define STACKBD_REDIRECT_OFF 0
#define STACKBD_REDIRECT_ON  1
#define STACKBD_BDEV_MODE (FMODE_READ | FMODE_WRITE | FMODE_EXCL)
#define KERNEL_SECTOR_SIZE 512
#define STACKBD_DO_IT _IOW( 0xad, 0, char * )
#define STACKBD_NAME "stackbd"
#define STACKBD_NAME_0 STACKBD_NAME "0"

typedef struct stackbd_s{
    sector_t capacity;
    struct gendisk *gd;
    spinlock_t lock;
    struct bio_list bio_list;
    struct task_struct *thread;
    int is_active;
    struct block_device *bdev_raw;
    struct request_queue *queue;
    atomic_t redirect_done;
}stackbd_t;

static stackbd_t stackbd;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
blk_qc_t stackbd_make_request(struct request_queue *q, struct bio *bio);
#else
void stackbd_make_request(struct request_queue *q,struct bio *bio);
#endif

void disk_write(struct rdma_ctx *ctx, struct bio *cloned_bio, unsigned int io_size, size_t start_index);
void _stackbd_make_request_bio(struct bio *bio);
void disk_write_bio_clone(struct request *req);
void _stackbd_make_request_bio_clone(struct request_queue *q, struct request *req);

int stackbd_init(void);
void stackbd_exit(void);
void cleanup_stackbd_queue(void);

#endif /* DISKBOX_H */
