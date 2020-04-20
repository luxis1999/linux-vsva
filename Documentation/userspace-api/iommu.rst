.. SPDX-License-Identifier: GPL-2.0
.. iommu:

=====================================
IOMMU Userspace API
=====================================

For bare-metal usages, IOMMU is a system device which does not need to
communicate with user space. However, it is needed for virtualization
use cases that involve guest Shared Virtual Addressing (SVA) and guest
IO virtual address (IOVA) which is based on physical IOMMU (pIOMMU)
nesting capability. Both guest IOVA and SVA requires a virtual IOMMU
(vIOMMU) to talk with the pIOMMU in the host.

IOMMU UAPI is designed to facilitate the communications between virtual
and physical IOMMUs for nesting IOMMU.

.. contents:: :local:


Functionality
====================================================
User space consumer of IOMMU UAPI should be aware of IOMMU and support
device model, domain, process address space ID (PASID), and many key
concepts. Communications between user and kernel are supported for both
directions. The supported APIs are as follows:

1. Alloc/Free PASID
2. Bind/unbind guest PASID (e.g. VT-d)
3. Bind/unbind guest PASID table (e.g. sMMU)
4. Invalidate IOMMU caches
5. Servicing page requests

Requirements
====================================================
The UAPI must support the following:

1. Emulated and paravirtualied vIOMMUs
2. Multiple vendors (Intel VT-d, ARM sMMU, etc.)
3. Kernel maintains backward compatibility and follow existing
   protocols to phase out features

Interfaces
====================================================
Moderate extensions to the IOMMU UAPI are needed to support the above
functionalities. This section covers the scheme for feature checking,
data passing, and the handling of future extensions.

Feature Checking
----------------------------------------------------
While launching a guest with vIOMMU, it is important to ensure that
host can support the UAPI data structures to be used for vIOMMU-pIOMMU
communications. Without the upfront compatibility checking, future
faults are difficult to report even in normal conditions. For example,
TLB invalidations should always succeed from vIOMMU's perspective.
There is no architectual way to report back the vIOMMU if the UAPI
data is not compatible. For this reason the following IOMMU UAPIs
cannot fail:

1. Free PASID
2. Unbind guest PASID
3. Unbind guest PASID table (SMMU)
4. Cache invalidate
5. Page response

User applications such as QEMU is expected to import kernel UAPI
headers. Only backward compatibility is supported. For example, an
older QEMU (with older kernel header), can run on newer kernel. Newer
QEMU (with new kernel header) may fail on older kernel.

User space consumer of IOMMU UAPI should check availability of the
features listed below before using it.
1) IOMMU_NESTING_FEAT_SYSWIDE_PASID
2) IOMMU_NESTING_FEAT_BIND_PGTBL
3) IOMMU_NESTING_FEAT_BIND_PASID_TABLE
4) IOMMU_NESTING_FEAT_CACHE_INVLD
5) IOMMU_NESTING_FEAT_PAGE_REQUEST

Also as nesting support relies on physcial IOMMU, user space consumer
should check the nesting realted info from host IOMMU before setting
nesting usage.
1) first level/stage format (e.g. INTEL_VTD)
2) vedonr specific cap infos (e.g. Intel VT-d nesting related capabilities)

User space could get the feature list and nesting related info by asking
host IOMMU driver to get iommu_nesting_info. User space should check the
nesting info per use case. Like below usages:
1) Using guest page table as 1st level/stage for Intel VT-d:
   - guest page table should be compatible with pIOMMU
   - host should support BIND_PGTBL and CACHE_INVLD
   - optional: PAGE_REQUEST
2) Support SVA in guest for Intel VT-d:
   - guest page table should be compatible with pIOMMU
   - host should support SYSWIDE_PASID, BIND_PGTBL and CACHE_INVLD
   - optional: PAGE_REQUEST
3) ...

Data Passing
----------------------------------------------------
In order to determine the size and feature set of the user data, size
and flags are embedded in the IOMMU UAPI data structures. A "__u32 size"
field is *always* at the beginning of each structure.

For example:
::

   struct iommu_gpasid_bind_data {
	__u32 size;
	__u32 version;
	__u32 format;
	#define IOMMU_SVA_GPASID_VAL	(1 << 0)
	__u64 flags;
	__u64 gpgd;
	__u64 hpasid;
	__u64 gpasid;
	__u32 addr_width;
	__u8  padding[12];
	/* Vendor specific data */
	union {
		struct iommu_gpasid_bind_data_vtd vtd;
	};
  };

When IOMMU APIs get extended, the data structures can *only* be
modified in two ways:

1. Adding new fields by repurposing the padding[] field. No size change.
2. Adding new union members at the end. May increase size.

No new fields can be added *after* the variable size union. In both
ways, a new flag must be accompanied with a new field such that the
IOMMU driver can process the data based on the new flag. Version field
is only reserved for the unlikely event of UAPI upgrade at its entirety.

It's *always* the caller's responsibility to indicate the size of the
structure passed by setting @size appropriately.

When IOMMU UAPI entension results in size increase, kernel has to handle
the following scenarios:

0. User and kernel has exact size match
1. An older user with older kernel header (smaller UAPI size) running
   on a newer kernel (larger UAPI size)
2. A newer user with newer kernel header (larger UAPI size) running
   on a older kernel.
3. A malicious/misbehaving user pass illegal/invalid size but within
   range. The data may contain garbage.

So far, IOMMU driver doesn't have its own user interface, so the usage
of IOMMU UAPI is supposed to be wrapped by an UAPI which has its own user
interface. e.g. VFIO. It follows the pattern below:
::

   struct vfio_struct {
	__u32 argsz;
	__u32 flags;
	__u8  data[];
  };

Here @data[] is the IOMMU UAPI data structures.

Such UAPI should check do necessary sanity check before calling into IOMMU
sub-system. Take the VFIO bind guest PASID as an example, VFIO code shall
process IOMMU UAPI request as follows:

::

 1	struct vfio_struct hdr;
 2
 3	minsz = offsetofend(struct vfio_iommu_type1_bind, flags);
 4	if (copy_from_user(&hdr, (void __user *)arg, minsz))
 5		return -EFAULT;
 6
 7	/* Check VFIO argsz */
 8	if (hdr.argsz < minsz)
 9		return -EINVAL;
 10
 11	if ((hdr.argsz - minsz) < sizeof(u32)) {
 12		/* User data < iommu_minsz */
 13		return -EINVAL;
 14	}
 15
 16
 17	/* VFIO flags must be included in minsz */
 18	switch (hdr.flags) {
 19	case VFIO_IOMMU_BIND_GUEST_PGTBL:
 20		/*
 21		 * Get the current IOMMU bind GPASID data size,
 22		 * which accounted for the largest union member.
 23		 */
 24		data_size = sizeof(struct iommu_gpasid_bind_data);
 25		if ((hdr.argsz - minsz) > data_size) {
 26			/* User data > current kernel */
 27			return -E2BIG;
 28		}
 29
 30		data = kzalloc(data_size, GFP_KERNEL);
 21		if (!data)
 32			return -ENOMEM;
 33
 34		if (copy_from_user(data, (void __user *)(arg + minsz),
 35				   hdr.argsz - minsz)) {
 36			kfree(data);
 37			return -EFAULT;
 38		}
 39
 40		/*
 41		 * the first field of IOMMU uapi structure is the data size
 42		 * This is necessary if VFIO layer needs to parse the IOMMU
 43		 * UAPI data structures. If not, may delegate IOMMU driver.
 44		 */
 45		if (*(u32 *)data != (hdr.argsz - minsz)) {
 46			kfree(data);
 47			return -EINVAL;
 48		}
 49
 50		ret = iommu_sva_bind_gpasid(domain, dev, iommu_bind_data);
 51		break;

Case 1 is supported. Case 2 will fail with -E2BIG at line #20. Case
3 may result in other error processed by IOMMU vendor driver. However,
the damage shall not exceed the scope of the offending user.
