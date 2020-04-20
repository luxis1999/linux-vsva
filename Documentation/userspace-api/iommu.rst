.. SPDX-License-Identifier: GPL-2.0
.. iommu:

=====================================
IOMMU Userspace API
=====================================

IOMMU UAPI is used for virtualization cases where communications are
needed between physical and virtual IOMMU drivers. For native
usage, IOMMU is a system device which does not need to communicate
with user space directly.

The primary use cases are guest Shared Virtual Address (SVA) and
guest IO virtual address (IOVA), wherein a virtual IOMMU (vIOMMU) is
required to communicate with the physical IOMMU in the host.

.. contents:: :local:

Functionalities
===============
Communications of user and kernel involve both directions. The
supported user-kernel APIs are as follows:

1. Alloc/Free PASID
2. Bind/unbind guest PASID (e.g. Intel VT-d)
3. Bind/unbind guest PASID table (e.g. ARM sMMU)
4. Invalidate IOMMU caches
5. Service page requests

Requirements
============
The IOMMU UAPIs are generic and extensible to meet the following
requirements:

1. Emulated and para-virtualised vIOMMUs
2. Multiple vendors (Intel VT-d, ARM sMMU, etc.)
3. Extensions to the UAPI shall not break existing user space

Interfaces
==========
Although the data structures defined in IOMMU UAPI are self-contained,
there is no user API functions introduced. Instead, IOMMU UAPI is
designed to work with existing user driver frameworks such as VFIO.

Extension Rules & Precautions
-----------------------------
When IOMMU UAPI gets extended, the data structures can *only* be
modified in two ways:

1. Adding new fields by re-purposing the padding[] field. No size change.
2. Adding new union members at the end. May increase in size.

No new fields can be added *after* the variable sized union in that it
will break backward compatibility when offset moves. In both cases, a
new flag must be accompanied with a new field such that the IOMMU
driver can process the data based on the new flag. Version field is
only reserved for the unlikely event of UAPI upgrade at its entirety.

It's *always* the caller's responsibility to indicate the size of the
structure passed by setting argsz appropriately.
Though at the same time, argsz is user provided data which is not
trusted. The argsz field allows the user to indicate how much data
they're providing, it's still the kernel's responsibility to validate
whether it's correct and sufficient for the requested operation.

Compatibility Checking
----------------------
When IOMMU UAPI extension results in size increase, user such as VFIO
has to handle the following cases:

1. User and kernel has exact size match
2. An older user with older kernel header (smaller UAPI size) running on a
   newer kernel (larger UAPI size)
3. A newer user with newer kernel header (larger UAPI size) running
   on an older kernel.
4. A malicious/misbehaving user pass illegal/invalid size but within
   range. The data may contain garbage.

Feature Checking
----------------
While launching a guest with vIOMMU, it is important to ensure that host
can support the UAPI data structures to be used for vIOMMU-pIOMMU
communications. Without upfront compatibility checking, future faults
are difficult to report even in normal conditions. For example, TLB
invalidations should always succeed. There is no architectural way to
report back to the vIOMMU if the UAPI data is incompatible. If that
happens, in order to protect IOMMU iosolation guarantee, we have to
resort to not giving completion status in vIOMMU. This may result in
VM hang.

For this reason the following IOMMU UAPIs cannot fail:

1. Free PASID
2. Unbind guest PASID
3. Unbind guest PASID table (SMMU)
4. Cache invalidate

User applications such as QEMU is expected to import kernel UAPI
headers. Backward compatibility is supported per feature flags.
For example, an older QEMU (with older kernel header) can run on newer
kernel. Newer QEMU (with new kernel header) may refuse to initialize
on an older kernel if new feature flags are not supported by older
kernel. Simply recompile existing code with newer kernel header should
not be an issue in that only existing flags are used.

IOMMU vendor driver should report the below features to IOMMU UAPI
consumers (e.g. via VFIO).

1. IOMMU_NESTING_FEAT_SYSWIDE_PASID
2. IOMMU_NESTING_FEAT_BIND_PGTBL
3. IOMMU_NESTING_FEAT_BIND_PASID_TABLE
4. IOMMU_NESTING_FEAT_CACHE_INVLD
5. IOMMU_NESTING_FEAT_PAGE_REQUEST

Take VFIO as example, upon request from VFIO user space (e.g. QEMU),
VFIO kernel code shall query IOMMU vendor driver for the support of
the above features. Query result can then be reported back to the
user-space caller. Details can be found in
Documentation/driver-api/vfio.rst.


Data Passing Example with VFIO
------------------------------
As the ubiquitous userspace driver framework, VFIO is already IOMMU
aware and share many key concepts such as device model, group, and
protection domain. Other user driver frameworks can also be extended
to support IOMMU UAPI but it is outside the scope of this document.

In this tight-knit VFIO-IOMMU interface, the ultimate consumer of the
IOMMU UAPI data is the host IOMMU driver. VFIO facilitates user-kernel
transport, capability checking, security, and life cycle management of
process address space ID (PASID).

Unlike normal user data passed via VFIO UAPI IOTCL, IOMMU driver is the
ultimate consumer of its UAPI data. At VFIO layer, the IOMMU UAPI data
is wrapped in a VFIO UAPI data. It follows the
pattern below::

   struct {
	__u32 argsz;
	__u32 flags;
	__u8  data[];
   };

Here data[] contains the IOMMU UAPI data structures. VFIO has the
freedom to bundle the data as well as parse data size based on its own flags.

In order to determine the size and feature set of the user data, argsz
and flags are also embedded in the IOMMU UAPI data structures.
A "__u32 argsz" field is *always* at the beginning of each structure.

For example:
::

   struct iommu_cache_invalidate_info {
	__u32	argsz;
	#define IOMMU_CACHE_INVALIDATE_INFO_VERSION_1 1
	__u32	version;
	/* IOMMU paging structure cache */
	#define IOMMU_CACHE_INV_TYPE_IOTLB	(1 << 0) /* IOMMU IOTLB */
	#define IOMMU_CACHE_INV_TYPE_DEV_IOTLB	(1 << 1) /* Device IOTLB */
	#define IOMMU_CACHE_INV_TYPE_PASID	(1 << 2) /* PASID cache */
	#define IOMMU_CACHE_INV_TYPE_NR		(3)
	__u8	cache;
	__u8	granularity;
	__u8	padding[2];
	union {
		struct iommu_inv_pasid_info pasid_info;
		struct iommu_inv_addr_info addr_info;
	} granu;
   };

VFIO is responsible for checking its own argsz and flags then invokes
appropriate IOMMU UAPI functions. User pointer is passed to IOMMU
layer for further processing. The responsibilities are divided as
follows:

- Generic IOMMU layer checks argsz range and override out-of-range
  value. If the exact argsz is based on generic flags, they are checked
  here as well.

- Vendor IOMMU driver checks argsz based on vendor flags, UAPI data
  is consumed based on flags

Once again, use guest TLB invalidation as an example, argsz is based
on generic flags in the invalidation information. IOMMU generic code
shall process the UAPI data as the following:

::

 int iommu_cache_invalidate(struct iommu_domain *domain, struct device *dev,
			void __user *uinfo)
 {
	/* Current kernel data size is the max to be copied from user */
	maxsz = sizeof(struct iommu_cache_invalidate_info);
	memset((void *)&inv_info, 0, maxsz);

	/*
	 * No new spaces can be added before the variable sized union, the
	 * minimum size is the offset to the union.
	 */
	minsz = offsetof(struct iommu_cache_invalidate_info, granu);

	/* Copy minsz from user to get flags and argsz */
	if (copy_from_user(&inv_info, uinfo, minsz))
		return -EFAULT;

	/* Fields before variable size union is mandatory */
	if (inv_info.argsz < minsz)
		return -EINVAL;
	/*
	 * User might be using a newer UAPI header which has a larger data
	 * size, we shall support the existing flags within the current
	 * size.
	 */
	if (inv_info.argsz > maxsz)
		inv_info.argsz = maxsz;

	/* Checking the exact argsz based on generic flags */
	if (inv_info.granularity == IOMMU_INV_GRANU_ADDR &&
		inv_info.argsz != offsetofend(struct iommu_cache_invalidate_info,
					granu.addr_info))
		return -EINVAL;

	if (inv_info.granularity == IOMMU_INV_GRANU_PASID &&
		inv_info.argsz != offsetofend(struct iommu_cache_invalidate_info,
					granu.pasid_info))
		return -EINVAL;

	/* Copy the remaining user data _after_ minsz */
	if (copy_from_user((void *)&inv_info + minsz, uinfo + minsz,
				inv_info.argsz - minsz))
		return -EFAULT;

	return domain->ops->cache_invalidate(domain, dev, &inv_info);
 }
 Add a wrapper
   __iommu_unbind_( kernel data, same user data, kernel copy)

Notice that in this example, since union size is determined by generic
flags, all checking to argsz is validated in the generic IOMMU layer,
vendor driver does not need to check argsz. However, if union size is
based on vendor data, such as iommu_sva_bind_gpasid(), it will be
vendor driver's responsibility to validate the exact argsz.
