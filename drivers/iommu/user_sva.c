// SPDX-License-Identifier: GPL-2.0-only
/*
 * Support SVA (under IOMMU nested translation) for user space.
 *
 * Copyright (C) 2021 Intel Corporation.
 *     Author: Liu Yi L <yi.l.liu@intel.com>
 *
 */

#include <linux/iommu.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched/mm.h>
#include <linux/device.h>
#include <linux/miscdevice.h>

#define DRIVER_VERSION  "0.1"
#define DRIVER_AUTHOR   "Liu Yi L <yi.l.liu@intel.com>"
#define DRIVER_DESC     "IOMMU SVA support user space"

struct usva_ctx {
	refcount_t		refs;
	struct mutex		lock;
	struct device		*dev;
	struct iommu_domain	*domain;
};

static const struct file_operations usva_fops;

static struct usva_ctx *usva_fileget(struct file *file)
{
	struct usva_ctx *ctx;

	if (file->f_op != &usva_fops)
		return ERR_PTR(-EINVAL);

	ctx = file->private_data;
	refcount_inc(&ctx->refs);
	return ctx;
}

/**
 * usva_fdget - Acquires a reference to the internal nesting FD context.
 * @fd: [in] nesting fd file descriptor.
 *
 * Returns a pointer to the internal nesting fd context, otherwise the error
 * pointers returned by the following functions:
 *
 */
struct usva_ctx *usva_fdget(int fd)
{
	struct usva_ctx *ctx;
	struct fd f = fdget(fd);

	if (!f.file)
		return ERR_PTR(-EBADF);
	ctx = usva_fileget(f.file);
	fdput(f);

	return ctx;
}
EXPORT_SYMBOL_GPL(usva_fdget);

void usva_put(struct usva_ctx *ctx)
{
	if (refcount_dec_and_test(&ctx->refs))
		kfree(ctx);
}
EXPORT_SYMBOL_GPL(usva_put);

/**
 * Register device/domain to a SVA context
 */
int usva_register_device(struct usva_ctx *ctx,
			  struct device *dev,
			  struct iommu_domain *domain)
{
	int ret, nest = 0;

	ret = iommu_domain_get_attr(domain, DOMAIN_ATTR_NESTING, &nest);
	if (ret || !nest)
		return -ENOTTY;

	mutex_lock(&ctx->lock);
	if (ctx->dev || ctx->domain) {
		mutex_unlock(&ctx->lock);
		return -EBUSY;
	}

	/*
	 * Caller of this API is responsible to hold dev & domain
	 * reference and ensures unregistration before releasing them.
	 */
	ctx->dev = dev;
	ctx->domain = domain;
	mutex_unlock(&ctx->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(usva_register_device);

void usva_unregister_device(struct usva_ctx *ctx)
{
	mutex_lock(&ctx->lock);
	ctx->dev = NULL;
	ctx->domain = NULL;
	mutex_unlock(&ctx->lock);
}
EXPORT_SYMBOL_GPL(usva_unregister_device);

static int usva_fops_release(struct inode *inode, struct file *filep)
{
	struct usva_ctx *ctx = filep->private_data;

	filep->private_data = NULL;

	usva_put(ctx);

	return 0;
}

static int usva_fops_open(struct inode *inode, struct file *filep)
{
	struct usva_ctx *ctx;
	int ret = 0;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	refcount_set(&ctx->refs, 1);
	mutex_init(&ctx->lock);
	filep->private_data = ctx;

	return ret;
}

static int usva_get_info(struct iommu_domain *domain, unsigned long arg)
{
	struct iommu_sva_info info;
	int ret;

	info.argsz = sizeof(struct iommu_sva_info);
	ret = iommu_domain_get_attr(domain, DOMAIN_ATTR_USVA, &info);
	if (ret)
		return ret;

	return copy_to_user((void __user *)arg, &info, info.argsz) ? -EFAULT : 0;
}

static long usva_fops_unl_ioctl(struct file *filep,
				 unsigned int cmd, unsigned long arg)
{
	struct usva_ctx *ctx = filep->private_data;
	long ret = -EINVAL;

	if (!ctx)
		return ret;

	mutex_lock(&ctx->lock);

	if (!ctx->dev || !ctx->domain) {
		mutex_unlock(&ctx->lock);
		return -ENODEV;
	}

	switch (cmd) {
	case IOMMU_USVA_GET_INFO:
		ret = usva_get_info(ctx->domain, arg);
		break;
	case IOMMU_USVA_BIND_PGTBL:
	case IOMMU_USVA_UNBIND_PGTBL:
	case IOMMU_USVA_FLUSH_CACHE:
		ret = -ENOTTY;
		break;
	default:
		pr_err("Unsupported cmd %u\n", cmd);
		break;
	}

	mutex_unlock(&ctx->lock);
	return ret;
}

static const struct file_operations usva_fops = {
	.owner		= THIS_MODULE,
	.open		= usva_fops_open,
	.release	= usva_fops_release,
	.unlocked_ioctl	= usva_fops_unl_ioctl,
};

static struct miscdevice user_sva = {
	.name = "user_sva",
	.fops = &usva_fops,
	.nodename = "usva",
	.mode = S_IRUGO | S_IWUGO,
};

static int __init usva_init(void)
{
	int ret;

	user_sva.minor = MISC_DYNAMIC_MINOR;

	ret = misc_register(&user_sva);
	if (ret) {
		pr_err("user_sva: misc device register failed\n");
		return ret;
	}

	pr_info("user_sva: got minor %i\n", user_sva.minor);

	return 0;
}

static void __exit usva_exit(void)
{
	misc_deregister(&user_sva);
}

module_init(usva_init);
module_exit(usva_exit);

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
