// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Intel Corporation.
 *     Author: Liu Yi L <yi.l.liu@intel.com>
 *
 */

#include <linux/vfio.h>
#include <linux/eventfd.h>
#include <linux/file.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched/mm.h>

#define DRIVER_VERSION  "0.1"
#define DRIVER_AUTHOR   "Liu Yi L <yi.l.liu@intel.com>"
#define DRIVER_DESC     "PASID management for VFIO bus drivers"

#define VFIO_DEFAULT_PASID_QUOTA	1000
static int pasid_quota = VFIO_DEFAULT_PASID_QUOTA;
module_param_named(pasid_quota, pasid_quota, uint, 0444);
MODULE_PARM_DESC(pasid_quota,
		 " Set the quota for max number of PASIDs that an application is allowed to request (default 1000)");

struct vfio_mm_token {
	unsigned long long val;
};

struct vfio_mm {
	struct kref		kref;
	int			ioasid_sid;
	struct mutex		pasid_lock;
	struct list_head	next;
	struct vfio_mm_token	token;
};

static struct vfio_pasid {
	struct mutex		vfio_mm_lock;
	struct list_head	vfio_mm_list;
} vfio_pasid;

/* called with vfio.vfio_mm_lock held */
static void vfio_mm_release(struct kref *kref)
{
	struct vfio_mm *vmm = container_of(kref, struct vfio_mm, kref);

	list_del(&vmm->next);
	mutex_unlock(&vfio_pasid.vfio_mm_lock);
	ioasid_free_set(vmm->ioasid_sid, true);
	kfree(vmm);
}

void vfio_mm_put(struct vfio_mm *vmm)
{
	kref_put_mutex(&vmm->kref, vfio_mm_release, &vfio_pasid.vfio_mm_lock);
}
EXPORT_SYMBOL_GPL(vfio_mm_put);

static void vfio_mm_get(struct vfio_mm *vmm)
{
	kref_get(&vmm->kref);
}

struct vfio_mm *vfio_mm_get_from_task(struct task_struct *task)
{
	struct mm_struct *mm = get_task_mm(task);
	struct vfio_mm *vmm;
	unsigned long long val = (unsigned long long) mm;
	int ret;

	mutex_lock(&vfio_pasid.vfio_mm_lock);
	/* Search existing vfio_mm with current mm pointer */
	list_for_each_entry(vmm, &vfio_pasid.vfio_mm_list, next) {
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
	ret = ioasid_alloc_set((struct ioasid_set *) mm, pasid_quota,
			       &vmm->ioasid_sid);
	if (ret) {
		kfree(vmm);
		vmm = ERR_PTR(ret);
		goto out;
	}

	kref_init(&vmm->kref);
	vmm->token.val = val;
	mutex_init(&vmm->pasid_lock);

	list_add(&vmm->next, &vfio_pasid.vfio_mm_list);
out:
	mutex_unlock(&vfio_pasid.vfio_mm_lock);
	mmput(mm);
	return vmm;
}
EXPORT_SYMBOL_GPL(vfio_mm_get_from_task);

int vfio_mm_ioasid_sid(struct vfio_mm *vmm)
{
	return vmm->ioasid_sid;
}
EXPORT_SYMBOL_GPL(vfio_mm_ioasid_sid);

int vfio_pasid_alloc(struct vfio_mm *vmm, int min, int max)
{
	ioasid_t pasid;

	pasid = ioasid_alloc(vmm->ioasid_sid, min, max, NULL);

	return (pasid == INVALID_IOASID) ? -ENOSPC : pasid;
}
EXPORT_SYMBOL_GPL(vfio_pasid_alloc);

void vfio_pasid_free_range(struct vfio_mm *vmm,
			    ioasid_t min, ioasid_t max)
{
	ioasid_t pasid = min;

	if (min > max)
		return;

	/*
	 * IOASID core will notify PASID users (e.g. IOMMU driver) to
	 * teardown necessary structures depending on the to-be-freed
	 * PASID.
	 * Hold pasid_lock to avoid race with PASID usages like bind/
	 * unbind page tables to requested PASID.
	 */
	mutex_lock(&vmm->pasid_lock);
	for (; pasid <= max; pasid++)
		ioasid_free(pasid);
	mutex_unlock(&vmm->pasid_lock);
}
EXPORT_SYMBOL_GPL(vfio_pasid_free_range);

int vfio_mm_for_each_pasid(struct vfio_mm *vmm, void *data,
			   void (*fn)(ioasid_t id, void *data))
{
	int ret;

	mutex_lock(&vmm->pasid_lock);
	ret = ioasid_set_for_each_ioasid(vmm->ioasid_sid, fn, data);
	mutex_unlock(&vmm->pasid_lock);
	return ret;
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
	mutex_init(&vfio_pasid.vfio_mm_lock);
	INIT_LIST_HEAD(&vfio_pasid.vfio_mm_list);
	return 0;
}

static void __exit vfio_pasid_exit(void)
{
	WARN_ON(!list_empty(&vfio_pasid.vfio_mm_list));
}

module_init(vfio_pasid_init);
module_exit(vfio_pasid_exit);

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
