// SPDX-License-Identifier: GPL-2.0
/*
 * ab_chardev.c — /dev/afterburnerN + ioctls mirroring the kext's UC sMethods ABI
 * (UC_METHODS §18, FINDINGS §30): submitFrame/queryCompletion/bindFrame/
 * cardStatus/checkForSpace/unbindAll/getPercent.
 *
 * v1 model: single session per open. bindFrame allocates BOTH coherent DMA
 * regions at once (kext binds a per-slot output buffer; input arrives from VT's
 * IOMemoryDescriptor — we fold that into one call). The user then mmaps the
 * input region (offset 0) and output region (offset 1), writes the compressed
 * frame, submits, and reads decoded YUV422 straight out of the mapped output
 * buffer — c2h DMA lands there directly, no kernel copy.
 */

#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>

#include "include/ab.h"
#include "include/ab_regs.h"

#define AB_DEV_NAME	"afterburner"
#define AB_MAX_DEVICES 1

/* ------------------------------------------------------------------ *
 * ioctl ABI — mirrors the UC selector order (sMethods @0x15680)
 * ------------------------------------------------------------------ */

struct ab_ioc_submit {
	__u32 in_off;    /* offset into mapped input region */
	__u32 in_len;    /* compressed frame bytes */
	__u32 out_len;   /* decoded YUV422 size (w*h*2) */
	__u32 poll_ms;   /* completion window (0 = default 2000) */
	/* out: */
	__u32 status;    /* last handled completion word (+0x2044) */
	__u32 data_word; /* +0x44/+0x1044 payload of final completion */
};

struct ab_ioc_query { __u32 status; __u32 data_word; };  /* out */
struct ab_ioc_bind { __u32 in_size; __u32 out_size; };   /* bindFrame (v1: both) */
struct ab_ioc_status {
	__u32 fw_version;  /* BAR0+0x8 */
	__u32 ready;       /* decoder ready bit */
	__u32 dec_enable;  /* BAR0[hdr+4] */
	__u32 engine_ctrl; /* BAR1+0x2010 */
};
struct ab_ioc_space { __u32 in_flight; __u32 max_in_flight; };

#define AB_IOC_SUBMIT_FRAME _IOWR('A', 0, struct ab_ioc_submit) /* submitFrame   */
#define AB_IOC_QUERY_COMPL  _IOR ('A', 1, struct ab_ioc_query)  /* queryCompletion */
#define AB_IOC_BIND_FRAME   _IOWR('A', 2, struct ab_ioc_bind)   /* bindFrame     */
#define AB_IOC_CARD_STATUS  _IOR ('A', 3, struct ab_ioc_status) /* cardStatus    */
#define AB_IOC_CHECK_SPACE  _IOR ('A', 4, struct ab_ioc_space)  /* checkForSpace */
#define AB_IOC_UNBIND_ALL   _IO  ('A', 5)                       /* unbindAll     */
#define AB_IOC_GET_PERCENT  _IOR ('A', 6, __u32)                /* getPercent    */

/* ------------------------------------------------------------------ *
 * mmap — offset 0 = input region, offset 1 = output region.
 * PFN-based mapping (remap_pfn_range per page): kernel 7.x's new DMA API
 * hands coherent allocations with refcount 0 on all but the first page,
 * which fails vm_insert_page's validate_page_before_insert() (-EINVAL).
 * remap_pfn_range does no page refcounting — correct for device memory.
 * Works for non-contiguous allocations too (one call per page).
 *
 * PROTECTION: pgprot_noncached (UC), NOT the default WB vma->vm_page_prot.
 * The FPGA's c2h DMA writes are not snooped by the CPU caches (live test,
 * 2026-11-09: a WB-mapped view kept stale memset bytes at scattered
 * positions after completion — classic No-Snoop TLP behavior). UC makes
 * every read hit RAM and every store post straight to RAM; ab_ring()
 * issues wmb() before the doorbell so user stores are visible first.
 * ------------------------------------------------------------------ */

static int ab_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct ab_device *ab = filp->private_data;
	struct ab_buf *r;
	size_t size = vma->vm_end - vma->vm_start;
	unsigned long i;
	pgprot_t prot = pgprot_noncached(vma->vm_page_prot);
	int ret;

	if (size & ~PAGE_MASK)
		return -EINVAL;
	if (!size)
		return 0;

	if (vma->vm_pgoff == 0)
		r = &ab->dma.in;
	else if (vma->vm_pgoff == 1)
		r = &ab->dma.out;
	else {
		dev_err(&ab->pdev->dev, "mmap: bad pgoff %lu\n", (unsigned long)vma->vm_pgoff);
		return -EINVAL;
	}

	mutex_lock(&ab->dma.lock);
	if (!r->virt || size > r->size) {
		dev_err(&ab->pdev->dev, "mmap: region not bound or too big (%px vs %zu)\n",
			r->virt, r->size);
		mutex_unlock(&ab->dma.lock);
		return -EINVAL;
	}

	for (i = 0; i < size / PAGE_SIZE; i++) {
		unsigned long pfn = virt_to_phys((char *)r->virt + i * PAGE_SIZE) >> PAGE_SHIFT;

		ret = remap_pfn_range(vma, vma->vm_start + i * PAGE_SIZE, pfn,
				      PAGE_SIZE, prot);
		if (ret) {
			dev_err(&ab->pdev->dev, "mmap: remap_pfn_range page %lu failed (%d)\n",
				i, ret);
			mutex_unlock(&ab->dma.lock);
			return ret;
		}
	}
	mutex_unlock(&ab->dma.lock);
	dev_info(&ab->pdev->dev, "mmap: pgoff=%lu size=%zu mapped\n",
		 (unsigned long)vma->vm_pgoff, size);
	return 0;
}

/* ------------------------------------------------------------------ *
 * ioctls
 * ------------------------------------------------------------------ */

static long ab_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct ab_device *ab = filp->private_data;
	struct ab_dma_state *dm = &ab->dma;

	switch (cmd) {
	case AB_IOC_SUBMIT_FRAME: {
		struct ab_ioc_submit s;
		u32 status = 0, data_word = 0;
		int ret;

		if (copy_from_user(&s, (void __user *)arg, sizeof(s)))
			return -EFAULT;
		if (!dm->in.virt || !dm->out.virt)
			return -EINVAL;	/* bindFrame first */
		if ((u64)s.in_off + s.in_len > dm->in.size || s.out_len > dm->out.size)
			return -EINVAL;

		ret = ab_submit_frame(ab, dm->in.pa + s.in_off, s.in_len,
				      dm->out.pa, s.out_len, s.poll_ms,
				      &status, &data_word);
		s.status = status;
		s.data_word = data_word;
		if (copy_to_user((void __user *)arg, &s, sizeof(s)))
			return -EFAULT;
		return ret;
	}

	case AB_IOC_QUERY_COMPL: {
		struct ab_ioc_query q = { 0 };
		int ret = ab_query_completion(ab, &q.status, &q.data_word);

		if (ret == -EAGAIN)
			ret = 0;	/* nothing pending — not an error */
		else if (ret < 0)
			return ret;
		if (copy_to_user((void __user *)arg, &q, sizeof(q)))
			return -EFAULT;
		return 0;
	}

	case AB_IOC_BIND_FRAME: {
		struct ab_ioc_bind b;
		int ret = 0;

		if (copy_from_user(&b, (void __user *)arg, sizeof(b)))
			return -EFAULT;
		/* drop previous binding — chains + data regions (takes lock) */
		ab_dma_release(ab, true);
		mutex_lock(&dm->lock);
		if (b.in_size) {
			dm->in.virt = dma_alloc_coherent(&ab->pdev->dev, b.in_size,
							 &dm->in.pa, GFP_KERNEL);
			if (!dm->in.virt)
				ret = -ENOMEM;
			else
				dm->in.size = b.in_size;
		}
		if (ret == 0 && b.out_size) {
			dm->out.virt = dma_alloc_coherent(&ab->pdev->dev, b.out_size,
							  &dm->out.pa, GFP_KERNEL);
			if (!dm->out.virt)
				ret = -ENOMEM;
			else
				dm->out.size = b.out_size;
		}
		mutex_unlock(&dm->lock);
		return ret;
	}

	case AB_IOC_CARD_STATUS: {
		struct ab_ioc_status st;

		st.fw_version = ab_read(ab, AB_BAR0, AB_B0_FW_VERSION);
		st.ready = (ab_read(ab, AB_BAR0, ab->hdr_off + AB_DEC_READY) & 1);
		st.dec_enable = ab_read(ab, AB_BAR0, ab->hdr_off + AB_DEC_ENABLE);
		st.engine_ctrl = ab_read(ab, AB_BAR1, AB_B1_ENGINE_CTRL);
		if (copy_to_user((void __user *)arg, &st, sizeof(st)))
			return -EFAULT;
		return 0;
	}

	case AB_IOC_CHECK_SPACE: {
		struct ab_ioc_space sp = { dm->in_flight, AB_MAX_IN_FLIGHT };

		if (copy_to_user((void __user *)arg, &sp, sizeof(sp)))
			return -EFAULT;
		return 0;
	}

	case AB_IOC_UNBIND_ALL: {
		u32 s;

		/* chains + data regions + counters (takes lock) */
		ab_dma_release(ab, true);

		/* drain any pending completions (kext unbindAll semantics) */
		for (;;) {
			s = ab_read(ab, AB_BAR1, AB_B1_COMPLETION);
			if (!s)
				break;
			dev_info(&ab->pdev->dev, "unbindAll: draining status=0x%08x\n", s);
			/* handle via query path */
			ab_query_completion(ab, NULL, NULL);
			mdelay(5);
		}
		return 0;
	}

	case AB_IOC_GET_PERCENT: {
		u32 p = dm->last_done ? 100 : 0; /* v1 stub — no mid-frame progress signal */

		if (copy_to_user((void __user *)arg, &p, sizeof(p)))
			return -EFAULT;
		return 0;
	}
	}
	return -ENOTTY;
}

static int ab_open(struct inode *inode, struct file *filp)
{
	struct ab_device *ab = container_of(inode->i_cdev, struct ab_device, cdev);

	filp->private_data = ab;
	return nonseekable_open(inode, filp);
}

static const struct file_operations ab_fops = {
	.owner = THIS_MODULE,
	.open = ab_open,
	.unlocked_ioctl = ab_ioctl,
	.mmap = ab_mmap,
};

/* ------------------------------------------------------------------ *
 * registration (called from ab_pci.c probe/remove)
 * ------------------------------------------------------------------ */

static dev_t ab_devno;
static struct class *ab_class;

int ab_chardev_register(struct ab_device *ab)
{
	int ret;

	ret = alloc_chrdev_region(&ab_devno, 0, AB_MAX_DEVICES, AB_DEV_NAME);
	if (ret)
		return ret;

	ab_class = class_create(AB_DEV_NAME); /* kernel >=6.4: single-arg */
	if (IS_ERR(ab_class)) {
		ret = PTR_ERR(ab_class);
		goto out_devno;
	}

	cdev_init(&ab->cdev, &ab_fops);
	ab->cdev.owner = THIS_MODULE;
	ret = cdev_add(&ab->cdev, ab_devno, AB_MAX_DEVICES);
	if (ret)
		goto out_class;

	ab->devnode = device_create(ab_class, NULL, ab_devno, ab, "afterburner0");
	if (IS_ERR(ab->devnode)) {
		ret = PTR_ERR(ab->devnode);
		ab->devnode = NULL;
		goto out_cdev;
	}

	dev_info(&ab->pdev->dev, "/dev/afterburner0 registered\n");
	return 0;

out_cdev:
	cdev_del(&ab->cdev);
out_class:
	class_destroy(ab_class);
out_devno:
	unregister_chrdev_region(ab_devno, AB_MAX_DEVICES);
	return ret;
}

void ab_chardev_unregister(struct ab_device *ab)
{
	if (ab->devnode) {
		device_destroy(ab_class, ab_devno);
		ab->devnode = NULL;
	}
	cdev_del(&ab->cdev);
	class_destroy(ab_class);
	unregister_chrdev_region(ab_devno, AB_MAX_DEVICES);
}
