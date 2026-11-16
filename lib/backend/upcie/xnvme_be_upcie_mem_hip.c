// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#include <xnvme_be.h>
#include <xnvme_be_nosys.h>
#ifdef XNVME_BE_UPCIE_HIP_ENABLED
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <xnvme_be_upcie_hip.h>
#include <xnvme_hip_buf.h>
#include <xnvme_dev.h>
#include <xnvme_be_state.h>
#include <xnvme_buf.h>

static void *
xnvme_be_upcie_hip_buf_alloc(const struct xnvme_dev *dev, size_t nbytes, uint64_t *phys)
{
	const struct xnvme_be_upcie_state *state = &xnvme_be_state(dev)->upcie_state;
	void *buf;

	buf = hipmem_dma_malloc(&g_upcie_hip_rte.hip_heap, nbytes);
	if (!buf) {
		errno = ENOMEM;
		return NULL;
	}
	if (phys) {
		*phys = dmamem_va_to_iova(state->dmem, buf);
	}

	return buf;
}

static void
xnvme_be_upcie_hip_buf_free(const struct xnvme_dev *XNVME_UNUSED(dev), void *buf)
{
	hipmem_dma_free(&g_upcie_hip_rte.hip_heap, buf);
}

static int
xnvme_be_upcie_hip_buf_vtophys(const struct xnvme_dev *dev, void *buf, uint64_t *phys)
{
	const struct xnvme_be_upcie_state *state = &xnvme_be_state(dev)->upcie_state;

	*phys = dmamem_va_to_iova(state->dmem, buf);
	if (!*phys) {
		XNVME_DEBUG("FAILED: buf(%p) is neither heap nor registered", buf);
		return -EINVAL;
	}

	return 0;
}

/**
 * Buffer objects a served client has registered with the server
 *
 * ROCm exports the dma-buf of the whole buffer object a range lies in, and
 * small allocations share an object. The server is given an object once, and
 * the ranges registered inside it count on that.
 */
static struct {
	struct {
		char *base;
		size_t nbytes;
		struct xnvme_be_upcie_ctrlr *ctrlr;
		int refs;
	} *entries;
	int count;
	int capacity;
} g_objects;

static int
_object_find(const void *vaddr, const struct xnvme_be_upcie_ctrlr *ctrlr)
{
	for (int i = 0; i < g_objects.count; ++i) {
		const char *base = g_objects.entries[i].base;

		if ((g_objects.entries[i].ctrlr == ctrlr) && ((const char *)vaddr >= base) &&
		    ((const char *)vaddr < (base + g_objects.entries[i].nbytes))) {
			return i;
		}
	}

	return -1;
}

static int
_object_add(void *base, size_t nbytes, struct xnvme_be_upcie_ctrlr *ctrlr)
{
	if (g_objects.count == g_objects.capacity) {
		int capacity = g_objects.capacity ? 2 * g_objects.capacity : 16;
		void *grown =
			realloc(g_objects.entries, (size_t)capacity * sizeof(*g_objects.entries));

		if (!grown) {
			return -ENOMEM;
		}
		g_objects.entries = grown;
		g_objects.capacity = capacity;
	}

	g_objects.entries[g_objects.count].base = base;
	g_objects.entries[g_objects.count].nbytes = nbytes;
	g_objects.entries[g_objects.count].ctrlr = ctrlr;
	g_objects.entries[g_objects.count].refs = 1;
	g_objects.count++;

	return 0;
}

static int
_served_map(const struct xnvme_be_upcie_state *state, void *vaddr, size_t nbytes)
{
	const uint32_t page_size = (uint32_t)g_upcie_hip_rte.hip_config.device_pagesize;
	size_t object_nbytes = 0;
	void *base = NULL;
	int dmabuf_fd = -1;
	int at, err;

	if (((uintptr_t)vaddr & (page_size - 1)) || (nbytes & (page_size - 1))) {
		XNVME_DEBUG("FAILED: vaddr(%p) nbytes(%zu) not aligned to page_size(%u)", vaddr,
			    nbytes, page_size);
		return -EINVAL;
	}

	err = xnvme_hip_buf_export_dmabuf(vaddr, nbytes, &base, &object_nbytes, &dmabuf_fd);
	if (err) {
		return err;
	}

	at = _object_find(base, state->ctrlr);
	if (at >= 0) {
		close(dmabuf_fd);
		g_objects.entries[at].refs++;
		return 0;
	}

	err = xnvme_be_upcie_served_mem_map(state->dmem, state->ctrlr, dmabuf_fd, base,
					    object_nbytes, page_size);
	close(dmabuf_fd);
	if (err) {
		return err;
	}

	err = _object_add(base, object_nbytes, state->ctrlr);
	if (err) {
		xnvme_be_upcie_served_mem_unmap(state->dmem, state->ctrlr, base);
	}

	return err;
}

static int
_served_unmap(const struct xnvme_be_upcie_state *state, void *vaddr)
{
	void *base;
	int at;

	at = _object_find(vaddr, state->ctrlr);
	if (at < 0) {
		return -EINVAL;
	}
	if (--g_objects.entries[at].refs) {
		return 0;
	}

	base = g_objects.entries[at].base;
	g_objects.entries[at] = g_objects.entries[--g_objects.count];

	return xnvme_be_upcie_served_mem_unmap(state->dmem, state->ctrlr, base);
}

static int
_map(const struct xnvme_dev *dev, void *vaddr, size_t nbytes)
{
	const struct xnvme_be_upcie_state *state = &xnvme_be_state(dev)->upcie_state;

	if (!g_upcie_rte.connection.alive) {
		return xnvme_be_upcie_dmamem_map(state->dmem, vaddr, nbytes, NULL);
	}

	return _served_map(state, vaddr, nbytes);
}

static int
_unmap(const struct xnvme_dev *dev, void *vaddr)
{
	const struct xnvme_be_upcie_state *state = &xnvme_be_state(dev)->upcie_state;

	if (g_upcie_rte.connection.alive) {
		return _served_unmap(state, vaddr);
	}

	return xnvme_be_upcie_dmamem_unmap(state->dmem, vaddr);
}

static int
_add(const struct xnvme_dev *dev, void *vaddr, size_t nbytes)
{
	const struct xnvme_be_upcie_state *state = &xnvme_be_state(dev)->upcie_state;
	int err;

	err = xnvme_buf_reg_add(state->dmem, vaddr, nbytes, 0);
	if (err) {
		_unmap(dev, vaddr);
	}

	return err;
}

static int
xnvme_be_upcie_hip_buf_register(const struct xnvme_dev *dev, void *vaddr, size_t nbytes)
{
	const struct xnvme_be_upcie_state *state = &xnvme_be_state(dev)->upcie_state;
	int err;

	err = xnvme_be_upcie_buf_reg_ref(state->dmem, g_upcie_hip_rte.hip_heap.vaddr,
					 g_upcie_hip_rte.hip_heap.size, vaddr, nbytes);
	if (err != -ENOENT) {
		return err;
	}

	err = _map(dev, vaddr, nbytes);
	if (err) {
		return err;
	}

	return _add(dev, vaddr, nbytes);
}

static int
xnvme_be_upcie_hip_buf_unregister(const struct xnvme_dev *dev, void *vaddr)
{
	const struct xnvme_be_upcie_state *state = &xnvme_be_state(dev)->upcie_state;
	struct xnvme_buf_reg *last;
	int err;

	err = xnvme_be_upcie_buf_reg_unref(state->dmem, g_upcie_hip_rte.hip_heap.vaddr,
					   g_upcie_hip_rte.hip_heap.size, vaddr, &last);
	if (err || !last) {
		return err;
	}

	err = _unmap(dev, last->vaddr);
	if (err) {
		return err;
	}
	xnvme_buf_reg_remove(last);

	return 0;
}

static int
xnvme_be_upcie_hip_buf_get_alignment(const struct xnvme_dev *XNVME_UNUSED(dev), size_t *align)
{
	*align = g_upcie_hip_rte.hip_config.device_pagesize;

	return 0;
}

#endif

struct xnvme_be_mem g_xnvme_be_upcie_mem_hip = {
	.id = "upcie-hip",
#ifdef XNVME_BE_UPCIE_HIP_ENABLED
	.buf_alloc = xnvme_be_upcie_hip_buf_alloc,
	.buf_realloc = xnvme_be_nosys_buf_realloc,
	.buf_free = xnvme_be_upcie_hip_buf_free,
	.buf_vtophys = xnvme_be_upcie_hip_buf_vtophys,
	.buf_register = xnvme_be_upcie_hip_buf_register,
	.buf_unregister = xnvme_be_upcie_hip_buf_unregister,
	.buf_get_alignment = xnvme_be_upcie_hip_buf_get_alignment,
#else
	.buf_alloc = xnvme_be_nosys_buf_alloc,
	.buf_realloc = xnvme_be_nosys_buf_realloc,
	.buf_free = xnvme_be_nosys_buf_free,
	.buf_vtophys = xnvme_be_nosys_buf_vtophys,
	.buf_register = xnvme_be_nosys_buf_register,
	.buf_unregister = xnvme_be_nosys_buf_unregister,
#endif
};
