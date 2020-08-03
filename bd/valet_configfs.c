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
#include "rdmabox.h"

int created_portals = 0;

#define cgroup_to_valet_session(x) container_of(x, struct valet_session, session_cg)
#define cgroup_to_valet_device(x) container_of(x, struct valet_file, dev_cg)

static ssize_t device_attr_store(struct config_item *item,
    struct configfs_attribute *attr,
    const char *page, size_t count)
{
  struct valet_session *valet_session;
  struct valet_file *valet_device;
  char xdev_name[MAX_valet_DEV_NAME];
  ssize_t ret;

  valet_session = cgroup_to_valet_session(to_config_group(item->ci_parent));
  valet_device = cgroup_to_valet_device(to_config_group(item));

  sscanf(page, "%s", xdev_name);
  if(valet_file_find(valet_session, xdev_name)) {
    printk("valet[%s]:Device already exists: %s",__func__, xdev_name);
    return -EEXIST;
  }
  ret = valet_create_device(valet_session, xdev_name, valet_device); 
  if (ret) {
    printk("valet[%s]:failed to create device %s\n",__func__, xdev_name);
    return ret;
  }

  return count;
}

static ssize_t state_attr_show(struct config_item *item,
    struct configfs_attribute *attr,
    char *page)
{
  struct valet_file *valet_device;
  ssize_t ret;

  valet_device = cgroup_to_valet_device(to_config_group(item));

  ret = snprintf(page, PAGE_SIZE, "%s\n", valet_device_state_str(valet_device));

  return ret;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
static struct configfs_item_operations valet_device_item_ops = {
  .store_attribute = device_attr_store,
  .show_attribute = state_attr_show,
};
#endif

// bind in valet_device_item_attrs
static struct configfs_attribute device_item_attr = {
  .ca_owner       = THIS_MODULE,
  .ca_name        = "device",
  .ca_mode        = S_IWUGO,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
  .show		  = state_attr_show,
  .store	  = device_attr_store,
#endif
};
static struct configfs_attribute state_item_attr = {
  .ca_owner       = THIS_MODULE,
  .ca_name        = "state",
  .ca_mode        = S_IRUGO,

};

static struct configfs_attribute *valet_device_item_attrs[] = {
  &device_item_attr,
  &state_item_attr,
  NULL,
};

static struct config_item_type valet_device_type = {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
  .ct_item_ops    = &valet_device_item_ops,
#endif
  .ct_attrs       = valet_device_item_attrs,
  .ct_owner       = THIS_MODULE,
};

static struct config_group *valet_device_make_group(struct config_group *group,
    const char *name)
{
  struct valet_session *valet_session;
  struct valet_file *valet_file;

  printk("valet[%s]: name=%s\n", __func__, name);
  valet_file = kzalloc(sizeof(*valet_file), GFP_KERNEL);
  if (!valet_file) {
    printk("valet[%s]:valet_file alloc failed\n",__func__);
    return NULL;
  }

  spin_lock_init(&valet_file->state_lock);
  if (valet_set_device_state(valet_file, DEVICE_OPENNING)) {
    printk("valet[%s]:device %s: Illegal state transition %s -> openning\n",__func__,
	valet_file->dev_name,
	valet_device_state_str(valet_file));
    goto err;
  }

  sscanf(name, "%s", valet_file->dev_name);
  valet_session = cgroup_to_valet_session(group);
  spin_lock(&valet_session->devs_lock);
  list_add(&valet_file->list, &valet_session->devs_list);
  spin_unlock(&valet_session->devs_lock);

  config_group_init_type_name(&valet_file->dev_cg, name, &valet_device_type);

  return &valet_file->dev_cg;
err:
  kfree(valet_file);
  return NULL;
}

static void valet_device_drop(struct config_group *group, struct config_item *item)
{
  struct valet_file *valet_device;
  struct valet_session *valet_session;

  valet_session = cgroup_to_valet_session(group);
  valet_device = cgroup_to_valet_device(to_config_group(item));
  valet_destroy_device(valet_session, valet_device);
  kfree(valet_device);
}

static ssize_t portal_attr_store(struct config_item *citem,
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
    struct configfs_attribute *attr,
#endif
    const char *buf,size_t count)
{
  char rdma[MAX_PORTAL_NAME] = "rdma://" ;
  struct valet_session *valet_session;

  sscanf(strcat(rdma, buf), "%s", rdma);
  if(valet_session_find_by_portal(&g_valet_sessions, rdma)) {
    printk("valet[%s]:Portal already exists: %s",__func__, buf);
    return -EEXIST;
  }

  valet_session = cgroup_to_valet_session(to_config_group(citem));
  if (valet_session_create(rdma, valet_session)) {
    printk("valet[%s]: Couldn't create new session with %s\n",__func__, rdma);
    return -EINVAL;
  }

  return count;
}

static struct configfs_group_operations valet_session_devices_group_ops = {
  .make_group     = valet_device_make_group,
  .drop_item      = valet_device_drop,
};

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
static struct configfs_item_operations valet_session_item_ops = {
  .store_attribute = portal_attr_store,
};
#endif

static struct configfs_attribute portal_item_attr = {
  .ca_owner       = THIS_MODULE,
  .ca_name        = "portal",
  .ca_mode        = S_IWUGO,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
  .store  	  = portal_attr_store,
#endif	
};

static struct configfs_attribute *valet_session_item_attrs[] = {
  &portal_item_attr,
  NULL,
};

static struct config_item_type valet_session_type = {
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
  .ct_item_ops    = &valet_session_item_ops,
#endif
  .ct_attrs       = valet_session_item_attrs,
  .ct_group_ops   = &valet_session_devices_group_ops,
  .ct_owner       = THIS_MODULE,
};

static struct config_group *valet_session_make_group(struct config_group *group,
    const char *name)
{
  struct valet_session *valet_session;

  valet_session = kzalloc(sizeof(*valet_session), GFP_KERNEL);
  if (!valet_session) {
    printk("valet[%s]:failed to allocate IS session\n",__func__);
    return NULL;
  }

  INIT_LIST_HEAD(&valet_session->devs_list);
  spin_lock_init(&valet_session->devs_lock);
  mutex_lock(&g_lock);
  list_add(&valet_session->list, &g_valet_sessions);
  created_portals++;
  mutex_unlock(&g_lock);

  config_group_init_type_name(&(valet_session->session_cg), name, &valet_session_type);

  return &(valet_session->session_cg);

}

static void valet_session_drop(struct config_group *group, struct config_item *item)
{
  struct valet_session *valet_session;

  valet_session = cgroup_to_valet_session(to_config_group(item));
  valet_session_destroy(valet_session);
  kfree(valet_session);
}

static struct configfs_group_operations valet_group_ops = {
  .make_group     = valet_session_make_group,
  .drop_item      = valet_session_drop,
};

static struct config_item_type valet_item = {
  .ct_group_ops   = &valet_group_ops,
  .ct_owner       = THIS_MODULE,
};

static struct configfs_subsystem valet_subsys = {
  .su_group = {
    .cg_item = {
      .ci_namebuf = "valet",
      .ci_type = &valet_item,
    },
  },

};

int valet_create_configfs_files(void)
{
  int err = 0;

  config_group_init(&valet_subsys.su_group);
  mutex_init(&valet_subsys.su_mutex);

  err = configfs_register_subsystem(&valet_subsys);
  if (err) {
    printk("valet[%s]:Error %d while registering subsystem %s\n",__func__,
	err, valet_subsys.su_group.cg_item.ci_namebuf);
  }

  return err;
}

void valet_destroy_configfs_files(void)
{
  configfs_unregister_subsystem(&valet_subsys);
}
