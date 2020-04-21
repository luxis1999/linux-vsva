// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Intel Corporation.
 *     Author: Liu Yi L <yi.l.liu@intel.com>
 *
 */

#include <linux/vfio.h>
#include <linux/file.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched/mm.h>

#define DRIVER_VERSION  "0.1"
#define DRIVER_AUTHOR   "Liu Yi L <yi.l.liu@intel.com>"
#define DRIVER_DESC     "PASID management for VFIO bus drivers"

/*
 * The quota default is 1000 for this version based on a forecast.
 * This is a start, we may give administrator the ability to tune
 * this value in future.
 */
static int pasid_quota = 1000;
module_param_named(pasid_quota, pasid_quota, uint, 0444);
MODULE_PARM_DESC(pasid_quota,
		 "Set the quota for max number of PASIDs that an application is allowed to request (default 1000)");

struct vfio_mm_token {
	unsigned long long val;
};

struct vfio_mm {
	struct kref		kref;
	struct ioasid_set	*ioasid_set;
	struct mutex		pasid_lock;
	struct rb_root		pasid_list;
	struct list_head	next;
	struct vfio_mm_token	token;
};

static struct mutex		vfio_mm_lock;
static struct list_head		vfio_mm_list;

struct vfio_pasid {
	struct rb_node		node;
	ioasid_t		pasid;
};

static void vfio_remove_all_pasids(struct vfio_mm *vmm);

/* called with vfio_mm_lock held */
static void vfio_mm_release(struct kref *kref)
{
	struct vfio_mm *vmm = container_of(kref, struct vfio_mm, kref);

	list_del(&vmm->next);
	mutex_unlock(&vfio_mm_lock);
	vfio_remove_all_pasids(vmm);
	ioasid_set_put(vmm->ioasid_set);
	kfree(vmm);
}

void vfio_mm_put(struct vfio_mm *vmm)
{
	kref_put_mutex(&vmm->kref, vfio_mm_release, &vfio_mm_lock);
}
EXPORT_SYMBOL_GPL(vfio_mm_put);

static void vfio_mm_get(struct vfio_mm *vmm)
{
	kref_get(&vmm->kref);
}

struct vfio_mm *vfio_mm_get_from_task(struct task_struct *task)
{
	struct mm_struct *mm = get_task_mm(task);
	unsigned long long val = (unsigned long long)mm;
	struct ioasid_set *tmp;
	struct vfio_mm *vmm;

	mutex_lock(&vfio_mm_lock);
	/* Search existing vfio_mm with current mm pointer */
	list_for_each_entry(vmm, &vfio_mm_list, next) {
		if (vmm->token.val == val) {
			vfio_mm_get(vmm);
			goto out;
		}
	}

	vmm = kzalloc(sizeof(*vmm), GFP_KERNEL);
	if (!vmm) {
		vmm = ERR_PTR(-ENOMEM);
		goto out;
	}

	/*
	 * IOASID core provides a 'IOASID set' concept to track all
	 * PASIDs associated with a token. Here we use mm_struct as
	 * the token and create a IOASID set per mm_struct. All the
	 * containers of the process share the same IOASID set.
	 */
	tmp = ioasid_set_alloc(mm, pasid_quota, IOASID_SET_TYPE_MM);
	if (IS_ERR(tmp)) {
		int ret = PTR_ERR(tmp);

		kfree(vmm);
		vmm = ERR_PTR(ret);
		goto out;
	}

	vmm->ioasid_set = tmp;
	kref_init(&vmm->kref);
	vmm->token.val = val;
	mutex_init(&vmm->pasid_lock);
	vmm->pasid_list = RB_ROOT;

	list_add(&vmm->next, &vfio_mm_list);
out:
	mutex_unlock(&vfio_mm_lock);
	mmput(mm);
	return vmm;
}
EXPORT_SYMBOL_GPL(vfio_mm_get_from_task);

int vfio_mm_get_pasid_quota(struct vfio_mm *vmm)
{
	return pasid_quota;
}
EXPORT_SYMBOL_GPL(vfio_mm_get_pasid_quota);

struct ioasid_set *vfio_mm_ioasid_set(struct vfio_mm *vmm)
{
	return vmm->ioasid_set;
}
EXPORT_SYMBOL_GPL(vfio_mm_ioasid_set);

/*
 * Find PASID within @min and @max
 */
static struct vfio_pasid *vfio_find_pasid(struct vfio_mm *vmm,
					  ioasid_t min, ioasid_t max)
{
	struct rb_node *node = vmm->pasid_list.rb_node;

	while (node) {
		struct vfio_pasid *vid = rb_entry(node,
						struct vfio_pasid, node);

		if (max < vid->pasid)
			node = node->rb_left;
		else if (min > vid->pasid)
			node = node->rb_right;
		else
			return vid;
	}

	return NULL;
}

static void vfio_link_pasid(struct vfio_mm *vmm, struct vfio_pasid *new)
{
	struct rb_node **link = &vmm->pasid_list.rb_node, *parent = NULL;
	struct vfio_pasid *vid;

	while (*link) {
		parent = *link;
		vid = rb_entry(parent, struct vfio_pasid, node);

		if (new->pasid <= vid->pasid)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}

	rb_link_node(&new->node, parent, link);
	rb_insert_color(&new->node, &vmm->pasid_list);
}

static void vfio_unlink_pasid(struct vfio_mm *vmm, struct vfio_pasid *old)
{
	rb_erase(&old->node, &vmm->pasid_list);
}

static void vfio_remove_pasid(struct vfio_mm *vmm, struct vfio_pasid *vid)
{
	vfio_unlink_pasid(vmm, vid);
	ioasid_free(vmm->ioasid_set, vid->pasid);
	kfree(vid);
}

static void vfio_remove_all_pasids(struct vfio_mm *vmm)
{
	struct rb_node *node;

	mutex_lock(&vmm->pasid_lock);
	while ((node = rb_first(&vmm->pasid_list)))
		vfio_remove_pasid(vmm, rb_entry(node, struct vfio_pasid, node));
	mutex_unlock(&vmm->pasid_lock);
}

ioasid_t vfio_pasid_alloc(struct vfio_mm *vmm, ioasid_t min, ioasid_t max)
{
	ioasid_t pasid;
	struct vfio_pasid *vid;

	pasid = ioasid_alloc(vmm->ioasid_set, min, max, NULL);
	if (pasid == INVALID_IOASID)
		return pasid;

	vid = kzalloc(sizeof(*vid), GFP_KERNEL);
	if (!vid) {
		ioasid_free(vmm->ioasid_set, pasid);
		return INVALID_IOASID;
	}

	vid->pasid = pasid;

	mutex_lock(&vmm->pasid_lock);
	vfio_link_pasid(vmm, vid);
	mutex_unlock(&vmm->pasid_lock);

	return pasid;
}
EXPORT_SYMBOL_GPL(vfio_pasid_alloc);

void vfio_pasid_free_range(struct vfio_mm *vmm,
			   ioasid_t min, ioasid_t max)
{
	struct vfio_pasid *vid = NULL;

	/*
	 * IOASID core will notify PASID users (e.g. IOMMU driver) to
	 * teardown necessary structures depending on the to-be-freed
	 * PASID.
	 * Hold pasid_lock also avoids race with PASID usages like bind/
	 * unbind page tables to requested PASID.
	 */
	mutex_lock(&vmm->pasid_lock);
	while ((vid = vfio_find_pasid(vmm, min, max)))
		vfio_remove_pasid(vmm, vid);
	mutex_unlock(&vmm->pasid_lock);
}
EXPORT_SYMBOL_GPL(vfio_pasid_free_range);

void vfio_mm_for_each_pasid(struct vfio_mm *vmm, void *data,
			    void (*fn)(ioasid_t id, void *data))
{
	mutex_lock(&vmm->pasid_lock);
	ioasid_set_for_each_ioasid(vmm->ioasid_set, fn, data);
	mutex_unlock(&vmm->pasid_lock);
}
EXPORT_SYMBOL_GPL(vfio_mm_for_each_pasid);

void vfio_mm_pasid_lock(struct vfio_mm *vmm)
{
	mutex_lock(&vmm->pasid_lock);
}
EXPORT_SYMBOL_GPL(vfio_mm_pasid_lock);

void vfio_mm_pasid_unlock(struct vfio_mm *vmm)
{
	mutex_unlock(&vmm->pasid_lock);
}
EXPORT_SYMBOL_GPL(vfio_mm_pasid_unlock);

static int __init vfio_pasid_init(void)
{
	mutex_init(&vfio_mm_lock);
	INIT_LIST_HEAD(&vfio_mm_list);
	return 0;
}

static void __exit vfio_pasid_exit(void)
{
	/*
	 * VFIO_PASID is supposed to be referenced by VFIO_IOMMU_TYPE1
	 * and may be other module. once vfio_pasid_exit() is triggered,
	 * that means its user (e.g. VFIO_IOMMU_TYPE1) has been removed.
	 * All the vfio_mm instances should have been released. If not,
	 * means there is vfio_mm leak, should be a bug of user module.
	 * So just warn here.
	 */
	WARN_ON(!list_empty(&vfio_mm_list));
}

module_init(vfio_pasid_init);
module_exit(vfio_pasid_exit);

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
