// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 */

#include <linux/version.h>
#include <linux/rk-dma-heap.h>

#if KERNEL_VERSION(5, 10, 0) <= LINUX_VERSION_CODE
#include <linux/dma-map-ops.h>
#endif

#include "rknpu_drv.h"
#include "rknpu_ioctl.h"
#include "rknpu_mem.h"

#ifdef CONFIG_ROCKCHIP_RKNPU_DMA_HEAP

static void rknpu_mem_object_release(struct kref *kref)
{
	struct rknpu_mem_object *rknpu_obj =
		container_of(kref, struct rknpu_mem_object, refcount);

	if (rknpu_obj->kv_addr) {
		struct iosys_map map =
			IOSYS_MAP_INIT_VADDR(rknpu_obj->kv_addr);

		dma_buf_vunmap(rknpu_obj->dmabuf, &map);
		rknpu_obj->kv_addr = NULL;
	}

	dma_buf_unmap_attachment(rknpu_obj->attachment, rknpu_obj->sgt,
				 DMA_BIDIRECTIONAL);
	dma_buf_detach(rknpu_obj->dmabuf, rknpu_obj->attachment);

	if (!rknpu_obj->owner)
		dma_buf_put(rknpu_obj->dmabuf);

	kfree(rknpu_obj);
}

void rknpu_mem_object_get(struct rknpu_mem_object *rknpu_obj)
{
	kref_get(&rknpu_obj->refcount);
}

void rknpu_mem_object_put(struct rknpu_mem_object *rknpu_obj)
{
	kref_put(&rknpu_obj->refcount, rknpu_mem_object_release);
}

/*
 * obj_addr is a per-session token, not the object's kernel address: a
 * lookup only ever returns an object of the calling session, and the
 * returned reference keeps it alive while the caller uses it.
 */
struct rknpu_mem_object *rknpu_mem_object_find(struct rknpu_session *session,
					       u64 id)
{
	struct rknpu_device *rknpu_dev = session->rknpu_dev;
	struct rknpu_mem_object *entry;

	if (!id || id > U32_MAX)
		return NULL;

	spin_lock(&rknpu_dev->lock);
	list_for_each_entry(entry, &session->list, head) {
		if (entry->id == (u32)id) {
			rknpu_mem_object_get(entry);
			spin_unlock(&rknpu_dev->lock);
			return entry;
		}
	}
	spin_unlock(&rknpu_dev->lock);

	return NULL;
}

static u32 rknpu_mem_id_alloc(struct rknpu_session *session)
{
	struct rknpu_mem_object *entry;
	bool used;
	u32 id;

	lockdep_assert_held(&session->rknpu_dev->lock);

	do {
		id = ++session->last_id;
		if (!id)
			continue;

		used = false;
		list_for_each_entry(entry, &session->list, head) {
			if (entry->id == id) {
				used = true;
				break;
			}
		}
	} while (used);

	return id;
}

int rknpu_mem_create_ioctl(struct rknpu_device *rknpu_dev, struct file *file,
			   unsigned int cmd, unsigned long data)
{
	struct rknpu_mem_create args;
	int ret = -EINVAL;
	struct dma_buf_attachment *attachment;
	struct sg_table *table;
	struct dma_buf *dmabuf;
	struct rknpu_mem_object *rknpu_obj = NULL;
	struct rknpu_session *session = NULL;
	int fd;
	unsigned int in_size = _IOC_SIZE(cmd);
	unsigned int k_size = sizeof(struct rknpu_mem_create);
	char *k_data = (char *)&args;
	struct iosys_map map = { 0 };

	/* _IOC_SIZE is fully userspace-controlled; anything larger than the
	 * kernel struct would overflow the stack object below. */
	if (in_size > k_size) {
		LOG_ERROR("%s: invalid ioctl size: %u\n", __func__, in_size);
		return -EINVAL;
	}

	if (unlikely(copy_from_user(&args, (struct rknpu_mem_create *)data,
				    in_size))) {
		LOG_ERROR("%s: copy_from_user failed\n", __func__);
		ret = -EFAULT;
		return ret;
	}

	if (k_size > in_size)
		memset(k_data + in_size, 0, k_size - in_size);

	rknpu_obj = kzalloc(sizeof(*rknpu_obj), GFP_KERNEL);
	if (!rknpu_obj)
		return -ENOMEM;

	if (args.handle > 0) {
		fd = args.handle;

		dmabuf = dma_buf_get(fd);
		if (IS_ERR(dmabuf)) {
			ret = PTR_ERR(dmabuf);
			goto err_free_obj;
		}

		rknpu_obj->dmabuf = dmabuf;
		rknpu_obj->owner = 0;
	} else {
		/* Start test kernel alloc/free dma buf */
		dmabuf = rk_dma_heap_buffer_alloc(rknpu_dev->heap, args.size,
						  O_CLOEXEC | O_RDWR, 0x0,
						  dev_name(rknpu_dev->dev));
		if (IS_ERR(dmabuf)) {
			LOG_ERROR("dmabuf alloc failed, args.size = %llu\n",
				  args.size);
			ret = PTR_ERR(dmabuf);
			goto err_free_obj;
		}

		rknpu_obj->dmabuf = dmabuf;
		rknpu_obj->owner = 1;

		fd = dma_buf_fd(dmabuf, O_CLOEXEC | O_RDWR);
		if (fd < 0) {
			LOG_ERROR("dmabuf fd get failed\n");
			ret = -EFAULT;
			goto err_free_dma_buf;
		}
	}

	attachment = dma_buf_attach(dmabuf, rknpu_dev->dev);
	if (IS_ERR(attachment)) {
		LOG_ERROR("dma_buf_attach failed\n");
		ret = PTR_ERR(attachment);
		goto err_free_dma_buf;
	}

	table = dma_buf_map_attachment(attachment, DMA_BIDIRECTIONAL);
	if (IS_ERR(table)) {
		LOG_ERROR("dma_buf_map_attachment failed\n");
		dma_buf_detach(dmabuf, attachment);
		ret = PTR_ERR(table);
		goto err_free_dma_buf;
	}

	if (args.flags & RKNPU_MEM_KERNEL_MAPPING) {
		ret = dma_buf_vmap(dmabuf, &map);
		if (ret) {
			LOG_ERROR("dma_buf_vmap failed\n");
			goto err_detach_dma_buf;
		}
		rknpu_obj->kv_addr = map.vaddr;
	}

	rknpu_obj->size = PAGE_ALIGN(args.size);
	rknpu_obj->dma_addr = sg_dma_address(table->sgl);
	rknpu_obj->sgt = table;
	rknpu_obj->attachment = attachment;

	spin_lock(&rknpu_dev->lock);

	session = file->private_data;
	if (session)
		rknpu_obj->id = rknpu_mem_id_alloc(session);

	spin_unlock(&rknpu_dev->lock);

	if (!session) {
		ret = -EFAULT;
		goto err_unmap_kv_addr;
	}

	kref_init(&rknpu_obj->refcount);

	args.size = rknpu_obj->size;
	args.obj_addr = rknpu_obj->id;
	args.dma_addr = rknpu_obj->dma_addr;
	args.handle = fd;

	LOG_DEBUG("args.handle: %d, args.size: %lld, id: %u, dma_addr: %#llx\n",
		  args.handle, args.size, rknpu_obj->id,
		  (__u64)rknpu_obj->dma_addr);

	if (unlikely(copy_to_user((struct rknpu_mem_create *)data, &args,
				  in_size))) {
		LOG_ERROR("%s: copy_to_user failed\n", __func__);
		ret = -EFAULT;
		goto err_unmap_kv_addr;
	}

	spin_lock(&rknpu_dev->lock);
	list_add_tail(&rknpu_obj->head, &session->list);
	spin_unlock(&rknpu_dev->lock);

	return 0;

err_unmap_kv_addr:
	dma_buf_vunmap(rknpu_obj->dmabuf, &map);
	rknpu_obj->kv_addr = NULL;

err_detach_dma_buf:
	dma_buf_unmap_attachment(attachment, table, DMA_BIDIRECTIONAL);
	dma_buf_detach(dmabuf, attachment);

err_free_dma_buf:
	if (rknpu_obj->owner)
		rk_dma_heap_buffer_free(dmabuf);
	else
		dma_buf_put(dmabuf);

err_free_obj:
	kfree(rknpu_obj);

	return ret;
}

int rknpu_mem_destroy_ioctl(struct rknpu_device *rknpu_dev, struct file *file,
			    unsigned long data)
{
	struct rknpu_mem_object *rknpu_obj;
	struct rknpu_session *session = NULL;
	struct rknpu_mem_destroy args;
	int ret = -EFAULT;

	if (unlikely(copy_from_user(&args, (struct rknpu_mem_destroy *)data,
				    sizeof(struct rknpu_mem_destroy)))) {
		LOG_ERROR("%s: copy_from_user failed\n", __func__);
		ret = -EFAULT;
		return ret;
	}

	session = file->private_data;
	if (!session) {
		ret = -EFAULT;
		return ret;
	}

	rknpu_obj = rknpu_mem_object_find(session, args.obj_addr);
	if (!rknpu_obj)
		return -EINVAL;

	LOG_DEBUG("free id: %u, dma_addr: %#llx\n", rknpu_obj->id,
		  (__u64)rknpu_obj->dma_addr);

	spin_lock(&rknpu_dev->lock);
	if (list_empty(&rknpu_obj->head)) {
		/* another thread already destroyed this object */
		spin_unlock(&rknpu_dev->lock);
		rknpu_mem_object_put(rknpu_obj);
		return -EINVAL;
	}
	list_del_init(&rknpu_obj->head);
	spin_unlock(&rknpu_dev->lock);

	rknpu_mem_object_put(rknpu_obj);
	rknpu_mem_object_put(rknpu_obj);

	return 0;
}

/*
 * begin cpu access => for_cpu = true
 * end cpu access => for_cpu = false
 */
static void __maybe_unused rknpu_dma_buf_sync(
	struct rknpu_device *rknpu_dev, struct rknpu_mem_object *rknpu_obj,
	u32 offset, u32 length, enum dma_data_direction dir, bool for_cpu)
{
	struct device *dev = rknpu_dev->dev;
	struct sg_table *sgt = rknpu_obj->sgt;
	struct scatterlist *sg = sgt->sgl;
	dma_addr_t sg_dma_addr = sg_dma_address(sg);
	unsigned int len = 0;
	int i;

	for_each_sgtable_sg(sgt, sg, i) {
		unsigned int sg_offset, sg_left, size = 0;

		len += sg->length;
		if (len <= offset) {
			sg_dma_addr += sg->length;
			continue;
		}

		sg_left = len - offset;
		sg_offset = sg->length - sg_left;

		size = (length < sg_left) ? length : sg_left;

		if (for_cpu)
			dma_sync_single_range_for_cpu(dev, sg_dma_addr,
						      sg_offset, size, dir);
		else
			dma_sync_single_range_for_device(dev, sg_dma_addr,
							 sg_offset, size, dir);

		offset += size;
		length -= size;
		sg_dma_addr += sg->length;

		if (length == 0)
			break;
	}
}

int rknpu_mem_sync_ioctl(struct rknpu_device *rknpu_dev, struct file *file,
			 unsigned long data)
{
	struct rknpu_mem_object *rknpu_obj = NULL;
	struct rknpu_session *session = NULL;
	struct rknpu_mem_sync args;
#ifdef CONFIG_DMABUF_PARTIAL
	struct dma_buf *dmabuf;
#endif
	int ret = -EFAULT;

	if (unlikely(copy_from_user(&args, (struct rknpu_mem_sync *)data,
				    sizeof(struct rknpu_mem_sync)))) {
		LOG_ERROR("%s: copy_from_user failed\n", __func__);
		ret = -EFAULT;
		return ret;
	}

	session = file->private_data;
	if (!session) {
		ret = -EFAULT;
		return ret;
	}

	rknpu_obj = rknpu_mem_object_find(session, args.obj_addr);
	if (!rknpu_obj)
		return -EINVAL;

	/*
	 * rknpu_dma_buf_sync() walks the sg_table with u32 offset/length,
	 * so reject ranges beyond that; the bounds check keeps the sync
	 * inside the object either way.
	 */
	if (args.offset > rknpu_obj->size ||
	    args.size > rknpu_obj->size - args.offset ||
	    args.offset > U32_MAX || args.size > U32_MAX) {
		rknpu_mem_object_put(rknpu_obj);
		return -EINVAL;
	}

#ifndef CONFIG_DMABUF_PARTIAL
	if (args.flags & RKNPU_MEM_SYNC_TO_DEVICE) {
		rknpu_dma_buf_sync(rknpu_dev, rknpu_obj, args.offset, args.size,
				   DMA_TO_DEVICE, false);
	}
	if (args.flags & RKNPU_MEM_SYNC_FROM_DEVICE) {
		rknpu_dma_buf_sync(rknpu_dev, rknpu_obj, args.offset, args.size,
				   DMA_FROM_DEVICE, true);
	}
#else
	dmabuf = rknpu_obj->dmabuf;
	if (args.flags & RKNPU_MEM_SYNC_TO_DEVICE) {
		dmabuf->ops->end_cpu_access_partial(dmabuf, DMA_TO_DEVICE,
						    args.offset, args.size);
	}
	if (args.flags & RKNPU_MEM_SYNC_FROM_DEVICE) {
		dmabuf->ops->begin_cpu_access_partial(dmabuf, DMA_FROM_DEVICE,
						      args.offset, args.size);
	}
#endif

	rknpu_mem_object_put(rknpu_obj);

	return 0;
}

#endif
