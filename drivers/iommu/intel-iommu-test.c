/* Intel IOMMU test driver Based on pci-stub
 */
#define DEBUG
#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/pci.h>
#include <linux/iommu.h>
#include <linux/intel-iommu.h>
#include <linux/intel-svm.h>
#include <linux/ioasid.h>

static char ids[1024] __initdata;
struct page_req_dsc {
	u64 srr:1;
	u64 bof:1;
	u64 pasid_present:1;
	u64 lpig:1;
	u64 pasid:20;
	u64 bus:8;
	u64 private:23;
	u64 prg_index:9;
	u64 rd_req:1;
	u64 wr_req:1;
	u64 exe_req:1;
	u64 priv_req:1;
	u64 devfn:8;
	u64 addr:52;
};

module_param_string(ids, ids, sizeof(ids), 0);
MODULE_PARM_DESC(ids, "Initial PCI IDs to add to the vtd_test driver, format is "
		 "\"vendor:device[:subvendor[:subdevice[:class[:class_mask]]]]\""
		 " and multiple comma separated entries can be specified");


struct bind_info {
	struct iommu_domain *domain;
	struct iommu_gpasid_bind_data data;
};

#define PASIDPTR_MASK 0xFFFFFFFFFFFFFULL
#define TEST_PASIDPTR_UNBIND 1
#define TEST_INVALIDATE_ALL 2
#define TEST_PASID_BIND_MM 3
#define TEST_GPASID_BIND 4
#define TEST_GPASID_UNBIND 5
#define TEST_IOASID_REG 6
#define TEST_IOASID_UNREG 7
#define TEST_IOASID_FREE 8

#if 0
static int prq_default_notifier(struct notifier_block *nb, unsigned long val,
                               void *data)
{
       struct iommu_fault_event *event = (struct iommu_fault_event *)data;;

//       pr_info("%s %p count %llu\n", __func__, event, event ? event->msg.paddr : 0);
       return NOTIFY_DONE;
}

static struct notifier_block prq_nb = {
       .notifier_call  = prq_default_notifier,
       /* lowest prio, we want it to run last. */
       .priority       = 0,
};
#endif

static u32 ioasid1 = 1001;
static u32 ioasid2 = 2001;

static ioasid_t intel_ioasid_alloc1(ioasid_t min, ioasid_t max, void *data)
{
	return ioasid1++;
}

static ioasid_t intel_ioasid_alloc2(ioasid_t min, ioasid_t max, void *data)
{
	return ioasid2++;
}

static void intel_ioasid_free(ioasid_t ioasid, void *data)
{
	pr_debug("%s: %u\n", __func__, ioasid);
}

static struct ioasid_allocator_ops intel_iommu_ioasid_allocator1 = {
	.alloc = intel_ioasid_alloc1,
	.free = intel_ioasid_free,
};
static struct ioasid_allocator_ops intel_iommu_ioasid_allocator2 = {
	.alloc = intel_ioasid_alloc2,
	.free = intel_ioasid_free,
};
static struct ioasid_allocator_ops intel_iommu_ioasid_allocator22 = {
	.alloc = intel_ioasid_alloc2,
	.free = intel_ioasid_free,
};


static struct iommu_domain *domain;
static ioasid_t gpasid_test[2];
static ssize_t test_vtd_gapsid_table_ptr_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long num;
	struct iommu_group *group;
	int ret;
	struct bind_info bi;
	struct mm_struct *mm;
	struct iommu_sva *sva, *ssva;
	int flags = 0;
	int attr1 = 1;

	ret = kstrtoul(buf, 0, &num);
	if (ret)
		return ret;

	switch (num) {
	case TEST_PASID_BIND_MM:
		pr_debug("test bind mm\n");
		mm = get_task_mm(current);
		sva = iommu_sva_bind_device(dev, mm, &flags);
		if (IS_ERR(sva)) {
			dev_dbg(dev, "%s: bind failed\n", __func__);
			break;
		}
		pr_debug("test bind mm again supervisor\n");
		flags |= SVM_FLAG_SUPERVISOR_MODE;
		ssva = iommu_sva_bind_device(dev, NULL, &flags);
		if (IS_ERR(ssva)) {
			dev_dbg(dev, "%s: bind supervisor failed\n", __func__);
			break;
		}
		pr_debug("test get pasid from sva, %d\n",
			iommu_sva_get_pasid(sva));
		pr_debug("test get su pasid from sva, %d\n",
			iommu_sva_get_pasid(ssva));
		pr_debug("test unbind mm\n");
		iommu_sva_unbind_device(sva);
		iommu_sva_unbind_device(ssva);
		mmput(mm);
		break;
	case  TEST_IOASID_FREE:
		pr_debug("test free gpasid %d\n", gpasid_test[0]);
		ioasid_free(gpasid_test[0]);
		ioasid_free(gpasid_test[1]);
		break;
	case  TEST_GPASID_UNBIND:
		pr_debug("test unbind gpasid %d\n", gpasid_test[0]);
		if (domain) {
			ret = iommu_sva_unbind_gpasid(domain, dev, gpasid_test[0]);
			if (ret)
				pr_err("unbind vsvm failed %d\n", ret);
			//iommu_unregister_fault_notifier(group, &prq_nb);
//			iommu_unregister_device_fault_handler(dev);

			pr_debug("domain exist, unbind gpasid\n");
			iommu_detach_device(domain, dev);
			iommu_domain_free(domain);
			domain = NULL;
		}
		ioasid_free(gpasid_test[0]);
		ioasid_free(gpasid_test[1]);
		break;
	case  TEST_GPASID_BIND:
		pr_debug("test bind gpasid\n");
		/* TODO: only bind once for now */
		if (domain) {
			pr_warn("Already bound, try enter 5 to unbind\n");
			goto out;
		}

		domain = iommu_domain_alloc(&pci_bus_type);
		if (!domain) {
			pr_err("alloc domain failed\n");
			ret = -ENODEV;
			goto out;
		}

		group = iommu_group_get(dev);
		if (!group) {
			pr_err("no group found \n");
			ret = -ENODEV;
			iommu_domain_free(domain);
			domain = NULL;
			goto out;
		}
		ret = count;

		gpasid_test[0] = ioasid_alloc(NULL, 200, 3000, NULL);
		gpasid_test[1] = ioasid_alloc(NULL, 200, 3000, NULL);
		pr_debug("Allocated gpasid %u\n", gpasid_test[0]);
		pr_debug("Allocated gpasid %u\n", gpasid_test[1]);
		bi.data.version = 1;
		bi.data.argsz = sizeof(struct iommu_gpasid_bind_data);
		bi.data.format = IOMMU_PASID_FORMAT_INTEL_VTD;
		bi.data.flags |= IOMMU_SVA_GPASID_VAL;
		bi.data.gpgd = 0xdeadbeef;
		bi.data.addr_width = 48;
		bi.data.hpasid = gpasid_test[0];
		bi.data.gpasid = gpasid_test[0] + 100;
		bi.data.vtd.flags = IOMMU_SVA_VTD_GPASID_SRE | IOMMU_SVA_VTD_GPASID_EAFE;
		bi.data.vtd.pat = 0xa5a5a5;
		ret = iommu_attach_device(domain, dev);
		if (ret) {
			dev_err(dev, "attach device failed ret %d", ret);
			return ret;
		}

		ret = iommu_domain_set_attr(domain, DOMAIN_ATTR_NESTING, &attr1);
		if (ret) {
			dev_err(dev, "domain set attr nesting ret %d", ret);
			return ret;
		}

		ret = iommu_sva_bind_gpasid(domain, dev, &bi.data);

		if (ret) {
			pr_debug("Failed bind gpasid %llu %d\n", bi.data.hpasid, ret);
			iommu_detach_device(domain, dev);
			iommu_domain_free(domain);
			domain = NULL;
		}
//		iommu_register_fault_notifier(group, &prq_nb);
		iommu_group_put(group);

		break;
	case  TEST_IOASID_REG:
		ret = ioasid_register_allocator(&intel_iommu_ioasid_allocator1);
		pr_debug("Done register allocator1 %d\n", ret);
		ret = ioasid_register_allocator(&intel_iommu_ioasid_allocator2);
		pr_debug("Done register allocator2 %d\n", ret);
		ret = ioasid_register_allocator(&intel_iommu_ioasid_allocator22);
		pr_debug("Done register allocator22 %d\n", ret);
		break;
	case  TEST_IOASID_UNREG:
		ioasid_unregister_allocator(&intel_iommu_ioasid_allocator1);
		pr_debug("Done unregister allocator1 \n");
		ioasid_unregister_allocator(&intel_iommu_ioasid_allocator2);
		pr_debug("Done unregister allocator2 \n");
		ioasid_unregister_allocator(&intel_iommu_ioasid_allocator22);
		pr_debug("Done unregister allocator22 \n");
		break;


	default:
		pr_debug("Unknown cmd %lu Choose TEST_PASIDPTR_UNBIND 1\n TEST_INVALIDATE_ALL 2 \n TEST_PASID_BIND_MM 3\n TEST_GPASID_BIND 4 \n TEST_GPASID_UNBIND 5\n TEST_IOASID_REG\n TEST_IOASID_UNREG\n ", num);

	}
out:

	return count;
}

static ssize_t test_vtd_gapsid_table_ptr_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return 0;
}

static DEVICE_ATTR(vtd_gpasid_table_ptr, S_IRUGO|S_IWUSR,
	test_vtd_gapsid_table_ptr_show,
	test_vtd_gapsid_table_ptr_store);

/* Test various invalidation caches */
static struct iommu_cache_invalidate_info tinfo[IOMMU_CACHE_INV_TYPE_NR] =
{
	/* IOTLB with PASID, global */
	{
		.argsz = offsetofend(struct iommu_cache_invalidate_info, addr_info),
		.version = 1,
		.cache = IOMMU_CACHE_INV_TYPE_IOTLB,
		.granularity = IOMMU_INV_GRANU_ADDR,
		.addr_info.pasid = 101,
		.addr_info.addr = 0xbabefd000,
		.addr_info.granule_size = 4096,
		.addr_info.nb_granules = 2,
		.addr_info.flags = IOMMU_INV_ADDR_FLAGS_PASID,
	},
	{
		.argsz = sizeof(struct iommu_cache_invalidate_info),
		.version = 1,
		.cache = IOMMU_CACHE_INV_TYPE_DEV_IOTLB,
		.granularity = IOMMU_INV_GRANU_PASID,
		.addr_info.addr = 0xbabefdbadbad,
		.pasid_info.pasid = 102,
		.pasid_info.flags = IOMMU_INV_PASID_FLAGS_PASID,
	},
	{
		.version = 1,
		.argsz = offsetofend(struct iommu_cache_invalidate_info, addr_info),
		.cache = IOMMU_CACHE_INV_TYPE_IOTLB,
		.granularity = IOMMU_INV_GRANU_ADDR,
		.addr_info.pasid = 103,
		.addr_info.addr = 0xbabefacefff,
		.addr_info.granule_size = 4096,
		.addr_info.nb_granules = 4,
		.addr_info.flags = IOMMU_INV_ADDR_FLAGS_PASID,
	},
};


#if 0
static int intel_svm_notify(struct page_req_dsc *desc)
{
       struct iommu_fault_event event;
       struct pci_dev *pdev;
       struct device_domain_info *info;
       int ret = 0;
       struct iommu_domain *pdomain;

       pdev = pci_get_bus_and_slot(desc->bus, desc->devfn);
       if (!pdev) {
               pr_err("No PCI device found for PRQ %x:%x.%x\n",
                       desc->bus, PCI_SLOT(desc->devfn),
                       PCI_FUNC(desc->devfn));
               return -ENODEV;
       }

       pdomain = iommu_get_domain_for_dev(&pdev->dev);
	if (!pdomain) {
		pr_err("IOMMU domain for device found %x:%x.%x\n",
			desc->bus, PCI_SLOT(desc->devfn),
			PCI_FUNC(desc->devfn));
		return -ENODEV;
	}
	pr_debug("domain vs. pdomain %p:%p\n", domain, pdomain);
	info = pdev->dev.archdata.iommu;
	if (!info || !info->pasid_tbl_bound) {
		pr_info("PRQ device pasid table not bound, skip notification\n");
		goto out;
	}

	event.dev = &pdev->dev;
	event.addr = desc->addr;
	event.count = 1;

       return iommu_fault_notifier_call_chain(domain, &event);
out:
       return ret;
}
#endif

static ssize_t test_vtd_invalidate_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	unsigned long num;
	int ret, i;
	struct iommu_group *group;

	ret = kstrtoul(buf, 0, &num);
	if (ret)
		return ret;
	
	dev_dbg(dev, "%s %d\n", __func__, __LINE__);
	group = iommu_group_get(dev);
	if (!group) {
		pr_err("no group found \n");
		ret = -ENODEV;
		goto out;
	}
	dev_dbg(dev, "%s %d\n", __func__, __LINE__);

	if ((num == TEST_INVALIDATE_ALL) && domain) {
		for (i = 0; i < IOMMU_CACHE_INV_TYPE_NR; i++) {
			dev_dbg(dev, "%s %d\n", __func__, __LINE__);
			ret = iommu_cache_invalidate(domain, dev, &tinfo[i]);
			if (ret)
				pr_err("invalidation failed %d\n", ret);
		}
		goto out;
	}
	ret = count;
#if 0
	desc.bus = 0;
	desc.devfn = num;
	pr_debug("Test prq notifier devfn %lu\n", num);
	if (!intel_svm_notify(&desc))
		pr_info("Notify OK, no need for response now\n");
#endif			
	iommu_group_put(group);
out:
	return count;
}

static ssize_t test_vtd_invalidate_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return 0;
}

static DEVICE_ATTR(vtd_invalidate, S_IRUGO|S_IWUSR,
	test_vtd_invalidate_show,
	test_vtd_invalidate_store);

static int pci_vtd_test_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	int ret;

	dev_info(&dev->dev, "claimed by vtd_test\n");

	ret = device_create_file(&dev->dev, &dev_attr_vtd_gpasid_table_ptr);
	ret = device_create_file(&dev->dev, &dev_attr_vtd_invalidate);

	return ret;
}

static void pci_vtd_test_remove(struct pci_dev *pdev)
{
	device_remove_file(&pdev->dev, &dev_attr_vtd_gpasid_table_ptr);
	device_remove_file(&pdev->dev, &dev_attr_vtd_invalidate);
}

static struct pci_driver vtd_test_driver = {
	.name		= "pci_vtd_test",
	.id_table	= NULL,	/* only dynamic id's */
	.probe		= pci_vtd_test_probe,
	.remove = pci_vtd_test_remove,

};

static int __init pci_vtd_test_init(void)
{
	char *p, *id;
	int rc;

	rc = pci_register_driver(&vtd_test_driver);
	if (rc)
		return rc;

	/* no ids passed actually */
	if (ids[0] == '\0')
		return 0;

	/* add ids specified in the module parameter */
	p = ids;
	while ((id = strsep(&p, ","))) {
		unsigned int vendor, device, subvendor = PCI_ANY_ID,
			subdevice = PCI_ANY_ID, class = 0, class_mask = 0;
		int fields;

		if (!strlen(id))
			continue;

		fields = sscanf(id, "%x:%x:%x:%x:%x:%x",
				&vendor, &device, &subvendor, &subdevice,
				&class, &class_mask);

		if (fields < 2) {
			pr_warn("pci_vtd_test: invalid id string \"%s\"\n", id);
			continue;
		}

		pr_info("pci_vtd_test: add %04X:%04X sub=%04X:%04X cls=%08X/%08X\n",
			vendor, device, subvendor, subdevice, class, class_mask);

		rc = pci_add_dynid(&vtd_test_driver, vendor, device,
				subvendor, subdevice, class, class_mask, 0);
		if (rc)
			pr_warn("pci_vtd_test: failed to add dynamic id (%d)\n", rc);
	}

	return 0;
}

static void __exit pci_vtd_test_exit(void)
{
	pci_unregister_driver(&vtd_test_driver);
}

module_init(pci_vtd_test_init);
module_exit(pci_vtd_test_exit);

MODULE_LICENSE("GPL");

