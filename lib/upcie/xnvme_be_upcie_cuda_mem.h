// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#ifndef __INTERNAL_XNVME_BE_UPCIE_CUDA_MEM_H
#define __INTERNAL_XNVME_BE_UPCIE_CUDA_MEM_H
#ifdef XNVME_BE_UPCIE_CUDA_ENABLED

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/uio.h>

#ifndef _UPCIE_WITH_NVME
#define _UPCIE_WITH_NVME
#endif
#include <upcie/upcie_cuda.h>

/**
 * An externally mapped CUDA buffer registered via xnvme_mem_map()
 *
 * Unlike heap-allocated buffers, mapped buffers are allocated by the caller
 * (e.g. via cudaMalloc) and their physical addresses are resolved on demand
 * through the dma-buf interface.
 */
struct cudamem_mapping {
	uint64_t vaddr;
	size_t size;
	struct dmabuf dmabuf;
	size_t nphys;
	uint64_t *phys_lut;
	struct cudamem_config *config;
	struct cudamem_mapping *next;
};

/**
 * A resolved region (heap or mapping) backing a CUDA virtual address range
 *
 * Used by PRP builders to amortize the linked-list walk across multiple
 * lookups within the same buffer.
 */
struct cudamem_region {
	uint64_t base_vaddr; ///< First vaddr covered by phys_lut[0]
	size_t size;         ///< Total bytes covered by the region
	size_t nphys;        ///< Number of phys_lut entries
	uint64_t *phys_lut;  ///< LUT at device_pagesize granularity
	int device_pagesize; ///< Granularity of phys_lut (bytes per entry)
	int is_heap;         ///< Heap allocations are assumed physically contiguous
};

/**
 * Locate the region (heap or mapping) containing the given virtual address.
 *
 * On success, fills *region with a snapshot of the matching region's metadata.
 * Returns 0 on success, -EINVAL if vaddr is not in the heap or any mapping.
 */
static inline int
cudamem_locate(struct cudamem_heap *heap, struct cudamem_mapping *mappings, void *virt,
	       struct cudamem_region *region)
{
	uint64_t vaddr = (uint64_t)virt;

	if (vaddr >= heap->vaddr && vaddr < heap->vaddr + heap->size) {
		region->base_vaddr      = heap->vaddr;
		region->size            = heap->size;
		region->nphys           = heap->nphys;
		region->phys_lut        = heap->phys_lut;
		region->device_pagesize = heap->config->device_pagesize;
		region->is_heap         = 1;
		return 0;
	}

	for (struct cudamem_mapping *m = mappings; m; m = m->next) {
		if (vaddr >= m->vaddr && vaddr < m->vaddr + m->size) {
			region->base_vaddr      = m->vaddr;
			region->size            = m->size;
			region->nphys           = m->nphys;
			region->phys_lut        = m->phys_lut;
			region->device_pagesize = m->config->device_pagesize;
			region->is_heap         = 0;
			return 0;
		}
	}

	return -EINVAL;
}

/**
 * Translate an offset within a previously-located region to a physical address.
 */
static inline uint64_t
cudamem_region_offset_to_phys(const struct cudamem_region *region, size_t byte_offset)
{
	size_t page_idx       = byte_offset / region->device_pagesize;
	size_t in_page_offset = byte_offset % region->device_pagesize;

	return region->phys_lut[page_idx] + in_page_offset;
}

/**
 * Resolve a CUDA virtual address to its physical address.
 *
 * Returns 0 on success, -EINVAL if vaddr is not in the heap or any mapping.
 */
static inline int
cudamem_vtp(struct cudamem_heap *heap, struct cudamem_mapping *mappings, void *virt,
	    uint64_t *phys)
{
	struct cudamem_region region;
	int err;

	err = cudamem_locate(heap, mappings, virt, &region);
	if (err) {
		return err;
	}

	*phys = cudamem_region_offset_to_phys(&region, (uint64_t)virt - region.base_vaddr);
	return 0;
}

/**
 * Prepare PRPs for a contiguous CUDA data buffer that may live in the heap or
 * an externally mapped region.
 *
 * Heap allocations are assumed physically contiguous (matching the legacy
 * upcie behavior), so the per-page PRP entries are derived by arithmetic from
 * prp1. For mapped regions, no contiguity assumption is made and each page is
 * resolved through the region LUT.
 *
 * Caveats
 * -------
 * - Does *not* support PRP list chaining; only a single list page is constructed.
 * - The entire buffer must reside within a single region (heap or one mapping).
 *
 * @return 0 on success, -EINVAL if the buffer cannot be resolved to a region.
 */
static inline int
nvme_request_prep_command_prps_contig_cuda_mapped(struct nvme_request *request,
						  struct cudamem_heap *heap,
						  struct cudamem_mapping *mappings, void *dbuf,
						  size_t dbuf_nbytes, struct nvme_command *cmd)
{
	const uint64_t pagesize       = heap->config->pagesize;
	const uint64_t pagesize_shift = heap->config->pagesize_shift;
	const uint64_t npages         = (dbuf_nbytes + pagesize - 1) >> pagesize_shift;
	struct cudamem_region region;
	size_t base_offset;
	int err;

	assert(npages <= 1 + 512);

	err = cudamem_locate(heap, mappings, dbuf, &region);
	if (err) {
		return err;
	}
	base_offset = (uint64_t)dbuf - region.base_vaddr;

	cmd->prp1 = cudamem_region_offset_to_phys(&region, base_offset);

	if (npages == 1) {
		return 0;
	} else if (npages == 2) {
		cmd->prp2 = region.is_heap ? cmd->prp1 + pagesize
					   : cudamem_region_offset_to_phys(&region,
									   base_offset + pagesize);
	} else {
		uint64_t *prp_list = request->prp;

		cmd->prp2 = request->prp_addr;
		if (region.is_heap) {
			for (uint64_t i = 1; i < npages; ++i) {
				prp_list[i - 1] = cmd->prp1 + (i << pagesize_shift);
			}
		} else {
			for (uint64_t i = 1; i < npages; ++i) {
				prp_list[i - 1] = cudamem_region_offset_to_phys(
					&region, base_offset + (i << pagesize_shift));
			}
		}
	}

	return 0;
}

/**
 * Prepare PRPs for a CUDA iovec (scatter-gather) data buffer that may live in
 * the heap or externally mapped regions.
 *
 * Each iovec entry must be page-aligned. The region is located once per iovec
 * entry, so the cost is O(total_pages) plus one mapping-list walk per iovec.
 *
 * @return 0 on success, -EINVAL if any iovec base cannot be resolved.
 */
static inline int
nvme_request_prep_command_prps_iov_cuda_mapped(struct nvme_request *request,
					       struct cudamem_heap *heap,
					       struct cudamem_mapping *mappings,
					       struct iovec *dvec, size_t dvec_cnt,
					       struct nvme_command *cmd)
{
	const uint64_t pagesize = heap->config->pagesize;
	uint64_t *prp_list      = request->prp;
	size_t prp_idx          = 0;
	int err;

	for (size_t i = 0; i < dvec_cnt; ++i) {
		struct cudamem_region region;
		uint8_t *base    = dvec[i].iov_base;
		size_t remaining = dvec[i].iov_len;
		size_t offset    = 0;

		err = cudamem_locate(heap, mappings, base, &region);
		if (err) {
			return err;
		}

		if (i == 0) {
			cmd->prp1 = cudamem_region_offset_to_phys(
				&region, (uint64_t)base - region.base_vaddr);
			offset    = pagesize;
			remaining = (remaining > pagesize) ? remaining - pagesize : 0;
		}

		while (remaining > 0) {
			prp_list[prp_idx++] = cudamem_region_offset_to_phys(
				&region, (uint64_t)base + offset - region.base_vaddr);

			offset += pagesize;
			remaining = (remaining > pagesize) ? remaining - pagesize : 0;
		}
	}

	if (prp_idx == 1) {
		cmd->prp2 = prp_list[0];
	} else if (prp_idx > 1) {
		cmd->prp2 = request->prp_addr;
	}

	return 0;
}

#endif /* XNVME_BE_UPCIE_CUDA_ENABLED */
#endif /* __INTERNAL_XNVME_BE_UPCIE_CUDA_MEM_H */
