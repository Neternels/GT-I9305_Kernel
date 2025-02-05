/*
 * arch/arm/mach-exynos/secmem-allocdev.c
 *
 * Copyright (c) 2010-2011 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/device.h>
#include <linux/cma.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>

#include <asm/memory.h>
#include <asm/cacheflush.h>

#include <plat/devs.h>
#include <plat/pd.h>

#include <mach/secmem.h>
#include <mach/dev.h>

#define MFC_SEC_MAGIC_CHUNK0	0x13cdbf16
#define MFC_SEC_MAGIC_CHUNK1	0x8b803342
#define MFC_SEC_MAGIC_CHUNK2	0x5e87f4f5
#define MFC_SEC_MAGIC_CHUNK3	0x3bd05317

struct miscdevice secmem;
struct secmem_crypto_driver_ftn *crypto_driver;
#if defined(CONFIG_ION)
extern struct ion_device *ion_exynos;
#endif

#if defined(CONFIG_MACH_MIDAS)
static char *secmem_info[] = {
	"mfc",		/* 0 */
	"fimc",         /* 1 */
	"mfc-shm",      /* 2 */
	"sectbl",       /* 3 */
	"fimd",		/* 4 */
	NULL
};
#else
static char *secmem_info[] = {
	"mfc",		/* 0 */
	"fimc",		/* 1 */
	"mfc-shm",	/* 2 */
	"sectbl",	/* 3 */
	"video",	/* 4 */
	"fimd",		/* 5 */
	NULL
};
#endif

static bool drm_onoff = false;

#define SECMEM_IS_PAGE_ALIGNED(addr) (!((addr) & (~PAGE_MASK)))

#if defined(CONFIG_USE_MFC_CMA)
static void *drm_buffer;
static int use_drm_buffer;

static inline void free_drmbuf(void)
{
	if (likely(drm_buffer)) {
		kfree(drm_buffer);
		drm_buffer = NULL;
	}
}

static inline int alloc_drmbuf(void)
{
	use_drm_buffer = 0;
	drm_buffer = kmalloc(SZ_256K, GFP_KERNEL | GFP_DMA);

	return drm_buffer == NULL ? 0 : 1;
}

static inline void *get_drmbuf(unsigned int len)
{
	if (!use_drm_buffer && len <= SZ_256K) {
		use_drm_buffer = 1;
		return drm_buffer;
	}

	return NULL;
}

static inline int put_drmbuf(void *ptr)
{
	if (use_drm_buffer && ptr == drm_buffer) {
		use_drm_buffer = 0;
		return 1;
	}

	return 0;
}
#else
static inline void free_drmbuf(void) { }
static inline int alloc_drmbuf(void) { return 1; }
static inline void *get_drmbuf(unsigned int len) { return NULL; }
static inline int put_drmbuf(void *ptr) { return 0; }
#endif

static int secmem_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = vma->vm_end - vma->vm_start;

	BUG_ON(!SECMEM_IS_PAGE_ALIGNED(vma->vm_start));
	BUG_ON(!SECMEM_IS_PAGE_ALIGNED(vma->vm_end));

	vma->vm_flags |= VM_RESERVED;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	if (remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
				size, vma->vm_page_prot)) {
		printk(KERN_ERR "%s : remap_pfn_range() failed!\n", __func__);
		return -EAGAIN;
	}

	return 0;
}

static long secmem_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case SECMEM_IOC_CHUNKINFO:
	{
		struct cma_info info;
		struct secchunk_info minfo;
		char **mname;
		int nbufs = 0;

		for (mname = secmem_info; *mname != NULL; mname++)
			nbufs++;

		if (nbufs == 0)
			return -ENOMEM;

		if (copy_from_user(&minfo, (void __user *)arg, sizeof(minfo)))
			return -EFAULT;

		if (minfo.index < 0)
			return -EINVAL;

		if (minfo.index >= nbufs) {
			minfo.index = -1; /* No more memory region */
		} else {

			if (cma_info(&info, secmem.this_device,
					secmem_info[minfo.index]))
				return -EINVAL;

			minfo.base = info.lower_bound;
			minfo.size = info.total_size;
		}

		if (copy_to_user((void __user *)arg, &minfo, sizeof(minfo)))
			return -EFAULT;
	}
	break;

#if defined(CONFIG_ION) && defined(CONFIG_CPU_EXYNOS5250)
	case SECMEM_IOC_GET_FD_PHYS_ADDR:
	{
		struct ion_client *client;
		struct secfd_info fd_info;
		struct ion_fd_data data;
		size_t len;

		if (copy_from_user(&fd_info, (int __user *)arg,
					sizeof(fd_info)))
			return -EFAULT;

		client = ion_client_create(ion_exynos, -1, "DRM");
		if (IS_ERR(client))
			printk(KERN_ERR "%s: Failed to get ion_client of DRM\n",
				__func__);

		data.fd = fd_info.fd;
		data.handle = ion_import_fd(client, data.fd);
		printk(KERN_DEBUG "%s: fd from user space = %d\n",
				__func__, fd_info.fd);
		if (IS_ERR(data.handle))
			printk(KERN_ERR "%s: Failed to get ion_handle of DRM\n",
				__func__);

		if (ion_phys(client, data.handle, &fd_info.phys, &len))
			printk(KERN_ERR "%s: Failed to get phys. addr of DRM\n",
				__func__);

		printk(KERN_DEBUG "%s: physical addr from kernel space = %lu\n",
				__func__, fd_info.phys);

		ion_free(client, data.handle);
		ion_client_destroy(client);

		if (copy_to_user((void __user *)arg, &fd_info, sizeof(fd_info)))
			return -EFAULT;
		break;
	}
#endif
	case SECMEM_IOC_GET_DRM_ONOFF:
		if (copy_to_user((void __user *)arg, &drm_onoff, sizeof(int)))
			return -EFAULT;
		break;
	case SECMEM_IOC_SET_DRM_ONOFF:
	{
		int val = 0;

		if (copy_from_user(&val, (int __user *)arg, sizeof(int)))
			return -EFAULT;

		if (val) {
			if (drm_onoff == false) {
				drm_onoff = true;
				pm_runtime_forbid((*(secmem.this_device)).parent);
			} else
				printk(KERN_ERR "%s: DRM is already on\n", __func__);
		} else {
			if (drm_onoff == true) {
				drm_onoff = false;
				pm_runtime_allow((*(secmem.this_device)).parent);
			} else
				printk(KERN_ERR "%s: DRM is already off\n", __func__);
		}
		break;
	}
	case SECMEM_IOC_GET_CRYPTO_LOCK:
	{
		int i;
		int ret;

		if (crypto_driver) {
			for (i = 0; i < 100; i++) {
				ret = crypto_driver->lock();
				if (ret == 0)
					break;
				printk(KERN_ERR "%s : Retry to get sync lock.\n",
					__func__);
			}
			return ret;
		}
		break;
	}
	case SECMEM_IOC_RELEASE_CRYPTO_LOCK:
	{
		if (crypto_driver)
			return crypto_driver->release();
		break;
	}
	case SECMEM_IOC_GET_ADDR:
	{
		struct secmem_region region;

		if (copy_from_user(&region, (void __user *)arg,
					sizeof(struct secmem_region)))
			return -EFAULT;

		if (!region.len) {
			printk(KERN_ERR "Get secmem address size error. [size : %ld]\n", region.len);
			return -EFAULT;
		}

		region.virt_addr = kmalloc(region.len, GFP_KERNEL | GFP_DMA);
		if (!region.virt_addr) {
			region.virt_addr = get_drmbuf(region.len);
			if (!region.virt_addr) {
				printk(KERN_ERR "%s:Get memory address failed."
						" [size : %ld]\n", __func__,
						region.len);
				return -EFAULT;
			}
		}
		region.phys_addr = virt_to_phys(region.virt_addr);

		dma_map_single(secmem.this_device, region.virt_addr, region.len, DMA_TO_DEVICE);

		if (copy_to_user((void __user *)arg, &region,
					sizeof(struct secmem_region)))
			return -EFAULT;
		break;
	}
	case SECMEM_IOC_RELEASE_ADDR:
	{
		struct secmem_region region;

		if (copy_from_user(&region, (void __user *)arg,
					sizeof(struct secmem_region)))
			return -EFAULT;

		if (!region.virt_addr) {
			printk(KERN_ERR "Get secmem address error. [address : %x]\n", (uint32_t)region.virt_addr);
			return -EFAULT;
		}

		if (!put_drmbuf(region.virt_addr))
			kfree(region.virt_addr);
		break;
	}

	case SECMEM_IOC_MFC_MAGIC_KEY:
	{
		uint32_t mfc_shm_virtaddr;
		struct cma_info info;
		struct secchunk_info minfo;

#if defined(CONFIG_CPU_EXYNOS4212) || defined(CONFIG_CPU_EXYNOS4412)
		if (cma_info(&info, secmem.this_device, "mfc-shm"))
			return -EINVAL;
#elif defined(CONFIG_CPU_EXYNOS5250)
		if (cma_info(&info, secmem.this_device, "mfc_sh"))
			return -EINVAL;
#endif

		minfo.base = info.lower_bound;
		minfo.size = info.total_size;

		mfc_shm_virtaddr = (uint32_t)phys_to_virt(minfo.base);

		*(uint32_t *)(mfc_shm_virtaddr) = MFC_SEC_MAGIC_CHUNK0;
		*(uint32_t *)(mfc_shm_virtaddr + 0x4) = MFC_SEC_MAGIC_CHUNK1;
		*(uint32_t *)(mfc_shm_virtaddr + 0x8) = MFC_SEC_MAGIC_CHUNK2;
		*(uint32_t *)(mfc_shm_virtaddr + 0xC) = MFC_SEC_MAGIC_CHUNK3;
		break;
	}

	case SECMEM_IOC_TEXT_CHUNKINFO:
	{
		struct cma_info info;
		struct secchunk_info minfo;

		if (cma_info(&info, secmem.this_device, "fimc0"))
			return -EINVAL;

		minfo.base = info.lower_bound;
		minfo.size = info.total_size;

		printk("[minfo base] : 0x%x", minfo.base);
		printk("[minfo size] : 0x%x", minfo.size);

		if (copy_to_user((void __user *)arg, &minfo, sizeof(minfo)))
			return -EFAULT;
		break;
	}

	default:
		return -ENOTTY;
	}

	return 0;
}

void secmem_crypto_register(struct secmem_crypto_driver_ftn *ftn)
{
	crypto_driver = ftn;
}
EXPORT_SYMBOL(secmem_crypto_register);

void secmem_crypto_deregister(void)
{
	crypto_driver = NULL;
}
EXPORT_SYMBOL(secmem_crypto_deregister);

static struct file_operations secmem_fops = {
	.unlocked_ioctl = &secmem_ioctl,
	.mmap		= secmem_mmap,
};

static int __init secmem_init(void)
{
	int ret;
	secmem.minor = MISC_DYNAMIC_MINOR;
	secmem.name = "s5p-smem";
	secmem.fops = &secmem_fops;

	if (unlikely(!alloc_drmbuf()))
		return -ENOMEM;

	ret = misc_register(&secmem);
	if (ret) {
		free_drmbuf();
		return ret;
	}

	crypto_driver = NULL;

	pm_runtime_enable(secmem.this_device);

	return 0;
}

static void __exit secmem_exit(void)
{
	__pm_runtime_disable(secmem.this_device, false);
	misc_deregister(&secmem);
	free_drmbuf();
}

module_init(secmem_init);
module_exit(secmem_exit);
