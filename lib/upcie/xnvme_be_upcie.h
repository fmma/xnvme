// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#ifndef __INTERNAL_XNVME_BE_UPCIE_H
#define __INTERNAL_XNVME_BE_UPCIE_H
#include <pthread.h>
#include <stdbool.h>

#include <xnvme_be.h>
#include <xnvme_queue.h>

#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

struct xnvme_queue_upcie {
	struct xnvme_queue_base base;
	struct nvme_qpair qpair;
	uint8_t _rvds[168];
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_queue_upcie) == XNVME_BE_QUEUE_STATE_NBYTES,
		    "Incorrect size")

enum nvme_backend {
	NVME_BACKEND_SYSFS = 0,
	NVME_BACKEND_VFIO,
};

/**
 * Shared controller state, one per physical controller, managed by cref.
 */
struct xnvme_be_upcie_ctrlr {
	struct nvme_controller *ctrl;
	struct nvme_qpair sync; ///< Shared submission/completion queue for synchronous IOs
	struct vfio_ctx vfio;
	enum nvme_backend backend;

	/* Attach mode (XNVME_UPCIE_ATTACH): this process drives I/O qpairs handed
	 * out by a foreign controller owner instead of opening the controller. In
	 * that case `ctrl` is NULL; the qpairs live in `region`, their doorbells
	 * in `func`'s BAR0, and geometry comes from `adesc`'s Identify payloads.
	 * `sync` and per-queue qpairs are imported from adesc->qpairs[], handed out
	 * in order via next_qpair (index 0 is `sync`). */
	bool attach;
	struct pci_func func;
	struct hostmem_hugepage region;
	struct upcie_attach_desc *adesc;
	uint32_t next_qpair;

	int timeout_ms; ///< Command timeout; mirrors ctrl->timeout_ms, or CAP.TO in attach mode
};

/**
 * Per-device state embedded in xnvme_dev.be.state.
 * The first field (ctrlr) is the cref handle written by the platform.
 */
struct xnvme_be_upcie_state {
	struct xnvme_be_upcie_ctrlr *ctrlr; ///< Shared controller (first field for platform)

	uint8_t _rvds[120];
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_be_upcie_state) == XNVME_BE_STATE_NBYTES, "Incorrect size")

/**
 * State used across multiple instances of controllers/namespaces
 */
struct xnvme_be_upcie_rte {
	struct hostmem_config config;
	struct hostmem_heap heap;
	int is_initialized;
};

extern struct xnvme_be_upcie_rte g_upcie_rte;

extern struct xnvme_be_mem g_xnvme_be_upcie_mem;
extern struct xnvme_be_admin g_xnvme_be_upcie_admin;
extern struct xnvme_be_sync g_xnvme_be_upcie_sync;
extern struct xnvme_be_async g_xnvme_be_upcie_async;
extern struct xnvme_be_dev g_xnvme_be_upcie_dev;

int
xnvme_be_upcie_get_driver_name(const char *bdf, char *driver_name, size_t driver_name_len);

// Used by xnvme_be_upcie_cuda_dev.c
void
xnvme_be_upcie_dev_close(struct xnvme_dev *dev);
int
xnvme_be_upcie_dev_open(struct xnvme_dev *dev);
void *
xnvme_be_upcie_ctrlr_init(struct xnvme_dev *dev);
int
xnvme_be_upcie_ctrlr_term(void *handle);

// Hand out the next pre-created I/O qpair from an attached controller's pool,
// imported into *qp. Returns -ENOMEM when the pool is exhausted.
int
xnvme_be_upcie_attach_get_qpair(struct xnvme_be_upcie_ctrlr *ctrlr, struct nvme_qpair *qp);

// Used by xnvme_be_upcie_cuda_async.c
int
xnvme_be_upcie_queue_init(struct xnvme_queue *queue, int opts);
int
xnvme_be_upcie_queue_term(struct xnvme_queue *queue);
int
xnvme_be_upcie_queue_poke(struct xnvme_queue *queue, uint32_t max);
#endif /* __INTERNAL_XNVME_BE_UPCIE */
