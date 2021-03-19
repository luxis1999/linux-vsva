========================================
I/O Address Space ID (IOASID) Controller
========================================

Acronyms
--------
PASID:
	Process Address Space ID, defined by PCIe
SVA:
	Shared Virtual Address

Introduction
------------

IOASIDs are used to associate DMA requests with virtual address spaces. As
a system-wide limited¹ resource, its constraints are managed by the IOASIDs
cgroup subsystem. The specific use cases are:

1. Some user applications exhaust all the available IOASIDs thus depriving
   others of the same host.

2. System admins need to provision VMs based on their needs for IOASIDs,
   e.g. the number of VMs with assigned devices that perform DMA requests
   with PASID.

The IOASID subsystem consists of three components:

- IOASID core: provides APIs for allocation, pool management,
  notifications and refcounting. See Documentation/driver-api/ioasid.rst
  for details
- IOASID user:  provides user allocation interface via /dev/ioasid
- IOASID cgroup controller: manage resource distribution

Resource Distribution Model
---------------------------
IOASID allocation is process-based in that IOASIDs are tied to page tables²,
the threaded model is not supported. The allocation is rejected by the
cgroup hierarchy once a limit is reached. However, organizational changes
such as moving processes across cgroups are exempted. Therefore, it is
possible to have ioasids.current > ioasids.max. It is not possible to do
further allocation after the organizational change that exceeds the max.

The system capacity of the IOASIDs is default to PCIe PASID size of 20 bits.
IOASID core provides API to adjust the system capacity based on platforms.
IOASIDs are used by both user applications (e.g. VMs and userspace drivers)
and kernel (e.g. supervisor SVA). However, only user allocation is subject
to cgroup constraints. Host kernel allocates a pool of IOASIDs where its
quota is subtracted from the system capacity. IOASIDs cgroup consults with
the IOASID core for available capacity when a new cgroup limit is granted.
Upon creation, no IOASID allocation is allowed by the user processes within
the new cgroup.

Usage
-----
CGroup filesystem has the following IOASIDs controller specific entries:
::

 ioasids.current
 ioasids.events
 ioasids.max

To use the IOASIDs controller, set ioasids.max to the limit of the number
of IOASIDs that can be allocated. The file ioasids.current shows the current
number of IOASIDs allocated within the cgroup.

Example
--------
1. Mount the cgroup2 FS ::

	$ mount -t cgroup2 none /mnt/cg2/

2. Add ioasids controller ::

	$ echo '+ioasids' > /mnt/cg2/cgroup.subtree_control

3. Create a hierarchy, set non-zero limit (default 0) ::

	$ mkdir /mnt/cg2/test1
	$ echo 5 > /mnt/cg2/test1/ioasids.max

4. Allocate IOASIDs within limit should succeed ::

	$echo $$ > /mnt/cg2/test1/cgroup.procs
	Do IOASID allocation via /dev/ioasid
	ioasids.current:1
	ioasids.max:5

5. Attempt to allocate IOASIDs beyond limit should fail ::

	ioasids.current:5
	ioasids.max:5

6. Attach a new process with IOASID already allocated to a cgroup could
result in ioasids.current > ioasids.max, e.g. process with PID 1234 under
a cgroup with IOASIDs controller has one IOASID allocated, moving it to
test1 cgroup ::

	$echo 1234 > /mnt/cg2/test1/cgroup.procs
	ioasids.current:6
	ioasids.max:5

Notes
-----
¹ When IOASID is used for PCI Express PASID, the range is limited to the
PASID size of 20 bits. For a device that its resources can be shared across
the platform, the IOASID namespace must be system-wide in order to uniquely
identify DMA request with PASID inside the device.

² The primary use case is SVA, where CPU page tables are shared with DMA via
IOMMU.
