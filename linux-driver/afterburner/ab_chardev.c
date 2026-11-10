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
#define AB_IOC_STOP         _IO  ('A', 7)                       /* stop (kext fn@0xc0a4) */
#define AB_IOC_START        _IO  ('A', 8)                       /* re-bring-up after STOP */

/* ------------------------------------------------------------------ *
 * Region allocation — single coherent block first; on no-IOMMU hosts a
 * large coherent alloc needs physically contiguous memory and fails
 * (order-N page alloc, ENOMEM). Fall back to chunked allocations: the chain
 * builder walks multi-segment lists so descriptors simply span chunks.
 * ------------------------------------------------------------------ */

static int ab_alloc_region(struct ab_device *ab, struct ab_buf *r, size_t size)
{
	r->virt = dma_alloc_coherent(&ab->pdev->dev, size, &r->pa, GFP_KERNEL);
	if (r->virt) {
		r->size = size;
		r->n_chunks = 1;
		r->chunks[0].pa = r->pa;
		r->chunks[0].virt = r->virt;
		r->chunks[0].len = size;
		return 0;
	}

	/* no-IOMMU fallback: several smaller coherent allocations.  The
	 * ladder works in whole pages — halving a byte size can land on a
	 * half-page boundary (odd page count), which breaks remap_pfn_range
	 * alignment downstream. */
	for (size_t cpages = min(size >> 1, (size_t)8 * PAGE_SIZE * 1024) / PAGE_SIZE;
	     cpages >= 256; cpages >>= 1) {
		size_t chunk = cpages * PAGE_SIZE;
		int n = (int)((size + chunk - 1) / chunk), i;
		size_t off = 0;

		if ((u32)n > AB_MAX_CHUNKS)
			continue;
		for (i = 0; i < n; i++) {
			size_t len = min(chunk, size - off);

			r->chunks[i].virt = dma_alloc_coherent(&ab->pdev->dev, len,
							       &r->chunks[i].pa, GFP_KERNEL);
			if (!r->chunks[i].virt)
				break;
			r->chunks[i].len = len;
			off += len;
		}
		if (i == n) {
			r->n_chunks = n;
			r->size = size;
			r->virt = r->chunks[0].virt;
			r->pa = r->chunks[0].pa;
			dev_info(&ab->pdev->dev,
				 "alloc: %zu B as %d x %zu B chunks (no-IOMMU fallback)\n",
				 size, n, chunk);
			return 0;
		}
		for (; i >= 0; i--)
			if (r->chunks[i].virt) {
				dma_free_coherent(&ab->pdev->dev, r->chunks[i].len,
						  r->chunks[i].virt, r->chunks[i].pa);
				r->chunks[i].virt = NULL;
			}
	}
	return -ENOMEM;
}

/* Slice [off, off+len) of a (possibly chunked) region into per-chunk
 * (pa,len) segments for the chain builder; at most max entries. */
static int ab_slice_segments(const struct ab_buf *r, u32 off, u32 len,
			     dma_addr_t *pas, size_t *lens, int max)
{
	int n = 0, i;
	u64 pos = 0, end = (u64)off + len;

	for (i = 0; i < r->n_chunks && n < max; i++) {
		const struct ab_chunk *c = &r->chunks[i];
		u64 cend = pos + c->len, s, e;

		if (cend <= off) {
			pos = cend;
			continue;
		}
		if (pos >= end)
			break;
		s = max((u64)off, pos);
		e = min(end, cend);
		pas[n] = c->pa + (s - pos);
		lens[n] = e - s;
		n++;
		pos = cend;
	}
	return n;
}

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

	/* walk chunks — a no-IOMMU fallback region spans several coherent
	 * allocations (FINDINGS §33); each chunk's pages map at its logical
	 * offset in the VMA */
	{
		u64 pos = 0;
		int ci;

		for (ci = 0; ci < r->n_chunks && pos < size; ci++) {
			const struct ab_chunk *c = &r->chunks[ci];
			u64 cend = pos + c->len, s = pos, e = min(cend, (u64)size);
			u64 p;

			for (p = s; p < e; p += PAGE_SIZE) {
				unsigned long pfn = virt_to_phys((char *)c->virt + (p - pos)) >> PAGE_SHIFT;

				ret = remap_pfn_range(vma, vma->vm_start + p, pfn,
						      PAGE_SIZE, prot);
				if (ret) {
					dev_err(&ab->pdev->dev, "mmap: remap_pfn_range at %llu failed (%d)\n",
						(unsigned long long)p, ret);
					mutex_unlock(&ab->dma.lock);
					return ret;
				}
			}
			pos = cend;
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
		dma_addr_t in_pas[AB_MAX_SEGS], out_pas[AB_MAX_SEGS];
		size_t in_lens[AB_MAX_SEGS], out_lens[AB_MAX_SEGS];
		int n_in, n_out;
		u32 status = 0, data_word = 0;
		int ret;

		if (copy_from_user(&s, (void __user *)arg, sizeof(s)))
			return -EFAULT;
		if (!dm->in.virt || !dm->out.virt)
			return -EINVAL;	/* bindFrame first */
		if ((u64)s.in_off + s.in_len > dm->in.size || s.out_len > dm->out.size)
			return -EINVAL;

		n_in = ab_slice_segments(&dm->in, s.in_off, s.in_len, in_pas, in_lens, AB_MAX_SEGS);
		n_out = ab_slice_segments(&dm->out, 0, s.out_len, out_pas, out_lens, AB_MAX_SEGS);
		if (!n_in || !n_out)
			return -EINVAL;

		{ /* truncation guard: sliced bytes must cover the request */
			u64 sum = 0;
			int k;

			for (k = 0; k < n_in; k++)
				sum += in_lens[k];
			if (sum != s.in_len)
				return -EINVAL;
			sum = 0;
			for (k = 0; k < n_out; k++)
				sum += out_lens[k];
			if (sum != s.out_len)
				return -EINVAL;
		}

		ret = ab_submit_frame(ab, in_pas, in_lens, n_in,
				      out_pas, out_lens, n_out,
				      s.in_len, s.out_len, s.poll_ms,
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
			size_t sz = (size_t)b.in_size + PAGE_SIZE - 1;

			sz &= ~(PAGE_SIZE - 1);
			ret = ab_alloc_region(ab, &dm->in, sz);
		}
		if (ret == 0 && b.out_size) {
			size_t sz = (size_t)b.out_size + PAGE_SIZE - 1;

			sz &= ~(PAGE_SIZE - 1);
			ret = ab_alloc_region(ab, &dm->out, sz);
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

	case AB_IOC_STOP: {
		/* kext stop() fn@0xc0a4 — decoder disable (BAR0[hdr+4]=0). Leaves the
		 * engine stopped until re-bring-up (rmmod/insmod or RECOVER); used by
		 * the V3 idle-AER test to hold the card quiescent with the driver
		 * still attached. Serialized against in-flight submits via dm->lock. */
		mutex_lock(&dm->lock);
		if (dm->in_flight) {
			mutex_unlock(&dm->lock);
			return -EBUSY; /* don't yank the decoder under a live frame */
		}
		ab_stop(ab);
		mutex_unlock(&dm->lock);
		return 0;
	}

	case AB_IOC_START: {
		/* Re-bring-up after STOP (or a wedged engine): decoder disable/enable
		 * cycle + init writer + ready poll — the kext's own recovery path
		 * (mode-8 rung A, §3n). Serialized against in-flight submits. */
		bool ok;

		mutex_lock(&dm->lock);
		if (dm->in_flight) {
			mutex_unlock(&dm->lock);
			return -EBUSY; /* don't cycle the decoder under a live frame */
		}
		ok = ab_recover(ab);
		mutex_unlock(&dm->lock);
		return ok ? 0 : -ETIMEDOUT;
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
