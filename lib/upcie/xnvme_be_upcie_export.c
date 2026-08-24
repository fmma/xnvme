// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

/**
 * Describing a runtime this process owns, so another can attach to it
 *
 * The backend knows things homi would otherwise have to reach in and take: the
 * heap's descriptor, the controller's BAR, and where in the heap a client's
 * description of both can live. This puts them behind one call, so the serving
 * loop stays about serving.
 *
 * Everything published here is written once, when the runtime comes up. What a
 * client does with it afterwards, and what it is allocated, is homi's business.
 */
#include <errno.h>
#include <string.h>

#include <libxnvme.h>
#include <xnvme_be.h>
#include <xnvme_dev.h>

#ifdef XNVME_BE_UPCIE_ENABLED
#include <xnvme_be_upcie.h>

/**
 * Place the record and the heap description in the heap, and report the rest
 *
 * @param dev A device this process opened
 * @param out Pre-allocated export to fill
 *
 * @return 0 on success, negative errno on error
 */
int
xnvme_be_upcie_export(struct xnvme_dev *dev, struct xnvme_be_upcie_export *out)
{
	struct xnvme_be_upcie_state *state;
	struct nvme_runtime_record *record;
	struct hostmem_shared_desc *desc;
	struct nvme_controller *ctrl;
	size_t record_offset, desc_offset, desc_nbytes;
	char *heap_base;
	int err;

	if (!dev || !out) {
		return -EINVAL;
	}

	state = (void *)dev->be.state;
	ctrl = state->ctrlr->ctrl;
	heap_base = g_upcie_rte.mem.dmem.base_va;

	if (!g_upcie_rte.mem.heap_alive || !heap_base) {
		XNVME_DEBUG("FAILED: no heap to describe");
		return -ENOTCONN;
	}

	/* Sized by what it has to carry: a base per granule where the device
	 * consumes physical addresses, and nothing beyond the header where it
	 * translates through an address space. */
	desc_nbytes = (g_upcie_rte.mode == XNVME_BE_UPCIE_MODE_UIO_LUT)
			      ? hostmem_shared_desc_nbytes(g_upcie_rte.mem.hp.nphys)
			      : sizeof(struct hostmem_shared_desc);

	err = dmamem_heap_alloc(&g_upcie_rte.mem.heap, desc_nbytes, &desc_offset);
	if (err) {
		XNVME_DEBUG("FAILED: dmamem_heap_alloc(desc); err(%d)", err);
		return err;
	}

	err = dmamem_heap_alloc(&g_upcie_rte.mem.heap, sizeof(*record), &record_offset);
	if (err) {
		XNVME_DEBUG("FAILED: dmamem_heap_alloc(record); err(%d)", err);
		dmamem_heap_free(&g_upcie_rte.mem.heap, desc_offset);
		return err;
	}

	desc = (struct hostmem_shared_desc *)(heap_base + desc_offset);
	record = (struct nvme_runtime_record *)(heap_base + record_offset);

	if (g_upcie_rte.mode == XNVME_BE_UPCIE_MODE_UIO_LUT) {
		err = hostmem_shared_desc_fill(desc, &g_upcie_rte.mem.hp);
	} else {
		err = hostmem_shared_desc_fill_arithmetic(desc, g_upcie_rte.mem.dmem.size,
							  g_upcie_rte.mem.dmem.base_iova);
	}
	if (err) {
		XNVME_DEBUG("FAILED: describing the heap; err(%d)", err);
		goto failed;
	}

	err = nvme_runtime_record_export(ctrl, g_upcie_rte.mem.dmem.size, record);
	if (err) {
		XNVME_DEBUG("FAILED: nvme_runtime_record_export(); err(%d)", err);
		goto failed;
	}
	record->desc_offset = desc_offset;

	memset(out, 0, sizeof(*out));
	/* Whichever descriptor actually backs the heap: the dmamem owns one
	 * where it allocated the memory itself, and wraps a hugepage where the
	 * device consumes physical addresses. Publishing the wrong one hands a
	 * client a descriptor it cannot map. */
	out->heap_fd =
		(g_upcie_rte.mem.dmem.fd >= 0) ? g_upcie_rte.mem.dmem.fd : g_upcie_rte.mem.hp.fd;
	out->bar0_fd = ctrl->func.bars[0].fd;
	out->bar0_nbytes = ctrl->func.bars[0].size;
	out->heap_nbytes = g_upcie_rte.mem.dmem.size;
	out->record_offset = record_offset;
	out->desc_offset = desc_offset;

	return 0;

failed:
	dmamem_heap_free(&g_upcie_rte.mem.heap, record_offset);
	dmamem_heap_free(&g_upcie_rte.mem.heap, desc_offset);

	return err;
}

/**
 * Release what an export took from the heap
 *
 * The record and the heap's description are allocated per export, so a process
 * that serves more than once would otherwise leave one of each behind per call.
 */
void
xnvme_be_upcie_unexport(struct xnvme_be_upcie_export *exported)
{
	if (!exported) {
		return;
	}

	if (exported->record_offset) {
		dmamem_heap_free(&g_upcie_rte.mem.heap, exported->record_offset);
	}
	if (exported->desc_offset) {
		dmamem_heap_free(&g_upcie_rte.mem.heap, exported->desc_offset);
	}

	memset(exported, 0, sizeof(*exported));
}
#endif
