// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#ifndef __INTERNAL_XNVME_BE_UPCIE_H
#define __INTERNAL_XNVME_BE_UPCIE_H
#include <pthread.h>
#include <stdatomic.h>

#include <xnvme_be.h>
#include <xnvme_queue.h>

#define _UPCIE_WITH_NVME
#include <upcie/upcie.h>

/**
 * Default heap size used for the host DMA heap (upcie) and the GPU device heap
 * (upcie-cuda) when xnvme_opts leaves the size unset (0)
 */
#define XNVME_BE_UPCIE_DEFAULT_HEAP_SIZE (1024ULL * 1024 * 1024)

/**
 * Heap offsets of a queue-pair's allocations
 *
 * Recorded at create so delete can hand the same offsets back to the heap.
 */
struct xnvme_be_upcie_qpair_offsets {
	size_t sq;  ///< Heap offset of the SQ
	size_t cq;  ///< Heap offset of the CQ
	size_t prp; ///< Heap offset of the per-request PRP scratch
};

struct xnvme_queue_upcie {
	struct xnvme_queue_base base;
	struct nvme_qpair qpair;
	struct xnvme_be_upcie_qpair_offsets offsets;
	uint8_t _rvds[144];
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_queue_upcie) == XNVME_BE_QUEUE_STATE_NBYTES,
		    "Incorrect size")

/**
 * How the target is attached, and with it how the DMA heap is wired up.
 *
 * Decided at ctrlr_init from the kernel driver bound to the BDF.
 */
enum xnvme_be_upcie_mode {
	XNVME_BE_UPCIE_MODE_UNSET = 0,
	XNVME_BE_UPCIE_MODE_VFIO_CDEV,  ///< vfio-cdev + iommufd + memfd
	XNVME_BE_UPCIE_MODE_UIO_LUT,    ///< uio_pci_generic + hugepages + LUT translator
	XNVME_BE_UPCIE_MODE_VFIO_TYPE1, ///< vfio-pci + legacy type1 container
};

/**
 * How one controller is attached, one member per mode
 *
 * Which member carries state follows xnvme_be_upcie_rte.mode; the rest stay
 * zeroed. Grouped so the controller struct says "attachment" once rather
 * than interleaving three modes' bookkeeping with everything else.
 */
struct xnvme_be_upcie_ctrlr_attach {
	struct nvme_dmamem_vfio_ctx vfio;   ///< VFIO_CDEV: vfio-cdev + iommufd state
	struct nvme_dmamem_uio_ctx uio;     ///< UIO_LUT: pci_bar_map state
	struct nvme_dmamem_type1_ctx type1; ///< VFIO_TYPE1: device fd state
	struct vfio_group type1_group;      ///< VFIO_TYPE1: owned per controller
	int type1_group_attached;           ///< Whether type1_group is set_container'd
};

/**
 * Shared controller state, one per physical controller, managed by cref.
 */
struct xnvme_be_upcie_ctrlr {
	struct nvme_controller *ctrl;
	struct xnvme_be_upcie_ctrlr_attach attach;
	struct nvme_qpair sync; ///< Shared submission/completion queue for synchronous IOs
	struct xnvme_be_upcie_qpair_offsets sync_offsets; ///< Heap offsets of the sync qpair
	struct nvme_request admin_prp; ///< PRP scratch for admin payloads, this controller's own
};

/**
 * Per-device state embedded in xnvme_dev.be.state.
 *
 * The first field (ctrlr) is the cref handle written by the platform. The
 * second says where data buffers for this device live: the host heap for
 * `upcie`, the GPU heap for `upcie-cuda` and `upcie-hip`, assigned by the
 * backend's dev_open. That is what lets the admin, sync and async paths be
 * shared across the three, since they differ only in which dmamem a payload
 * translates through.
 */
struct xnvme_be_upcie_state {
	struct xnvme_be_upcie_ctrlr *ctrlr; ///< Shared controller (first field for platform)
	struct dmamem *dmem;                ///< Where this device's data buffers live

	uint8_t _rvds[112];
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_be_upcie_state) == XNVME_BE_STATE_NBYTES, "Incorrect size")

#define XNVME_BE_UPCIE_GRANTS_MAX 64 ///< Queues this process will create for clients

/**
 * What a process needs to hand another one for it to attach
 *
 * The descriptors are the server's and stay open in it; a client receives
 * copies over a socket. The offsets are into the heap the descriptor names.
 */
struct xnvme_be_upcie_export {
	int heap_fd;            ///< The DMA heap, for the client to map
	int bar0_fd;            ///< BAR0, since a client rings its own doorbell
	uint64_t bar0_nbytes;   ///< How much of BAR0 to map
	uint64_t heap_nbytes;   ///< How much of the heap to map
	uint64_t record_offset; ///< Where the runtime record sits
	uint64_t desc_offset;   ///< Where the heap's description sits
	char uri[32];           ///< The identifier clients were given
};

int
xnvme_be_upcie_export(struct xnvme_dev *dev, struct xnvme_be_upcie_export *out);

/**
 * Release what xnvme_be_upcie_export() took from the heap
 *
 * Safe on an all-zero export, so a caller can release unconditionally after a
 * failed export.
 */
void
xnvme_be_upcie_unexport(struct xnvme_be_upcie_export *exported);

/**
 * A queue created for a client, described in terms it can resolve
 *
 * Plain data on purpose: the queue itself stays in the backend, so a tool
 * serving clients never handles a uPCIe structure.
 */
struct xnvme_be_upcie_qgrant {
	uint64_t sq_offset;  ///< Submission queue, as a heap offset
	uint64_t cq_offset;  ///< Completion queue, as a heap offset
	uint64_t prp_offset; ///< Scratch for the client's request pool
	uint32_t qid;        ///< The identifier allocated
	uint16_t depth;      ///< Entries in the queue pair
	uint16_t _rsvd;
};

int
xnvme_be_upcie_alloc_ioqpair(struct xnvme_dev *dev, uint16_t depth,
			     struct xnvme_be_upcie_qgrant *out);

int
xnvme_be_upcie_free_ioqpair(struct xnvme_dev *dev, uint32_t qid);

int
xnvme_be_upcie_admin(struct xnvme_dev *dev, void *cmd, void *cpl);

void
xnvme_be_upcie_socket_path(uint32_t shm_id, const char *bdf, char *path, size_t nbytes);

int
xnvme_be_upcie_ask(struct nvme_cplane_msg *msg, int *fds, uint32_t *nfds);

int
xnvme_be_upcie_attach(uint32_t shm_id, const char *bdf);

int
xnvme_be_upcie_query(uint32_t shm_id, const char *bdf, struct nvme_cplane_msg *msg);

void
xnvme_be_upcie_detach(void);

int
xnvme_be_upcie_attach_ctrlr(struct nvme_controller *ctrl);

int
xnvme_be_upcie_attach_qpair(struct nvme_qpair *qpair, uint16_t depth);

/**
 * The controller's I/O queue for this process, asked for if it has none yet
 *
 * An I/O queue is dedicated to whoever holds it, so an attached process takes
 * one only when it has something to submit. The admin queue is not this: that
 * one is the server's and shared, and commands for it travel over the socket.
 *
 * @return The queue on success, NULL on failure with errno set.
 */
struct nvme_qpair *
xnvme_be_upcie_ctrlr_ioq(struct xnvme_be_upcie_ctrlr *ctrlr);

struct nvme_request *
xnvme_be_upcie_ctrlr_admin_prp(struct xnvme_be_upcie_ctrlr *ctrlr);

void
xnvme_be_upcie_ctrlr_admin_prp_release(struct xnvme_be_upcie_ctrlr *ctrlr);

void
xnvme_be_upcie_detach_qpair(struct nvme_qpair *qpair);

int
xnvme_be_upcie_alloc_buf(size_t nbytes, uint64_t *offset);

int
xnvme_be_upcie_free_buf(uint64_t offset);

/**
 * State used across multiple instances of controllers/namespaces
 *
 * One dmamem_heap regardless of mode, so every controller allocates queues,
 * PRP scratch and data buffers from the same offset space. It sits on a
 * hostmem_hugepage whose borrowed phys_lut resolves addresses at submit time.
 */
/** VFIO_CDEV mode: one iommufd handle with one IOAS, shared by all controllers */
struct xnvme_be_upcie_rte_cdev {
	struct iommufd iommufd;
	int iommufd_alive; ///< Whether the iommufd handle is open
	int ioas_alive;    ///< Whether an IOAS is allocated on it
};

/** VFIO_TYPE1 mode: one legacy container, the first controller sets the iommu */
struct xnvme_be_upcie_rte_type1 {
	struct vfio_container container;
	int container_alive; ///< Whether the container is open
	int iommu_set;       ///< Whether VFIO_SET_IOMMU has been applied to it
};

/**
 * The DMA memory every controller allocates from, whatever the mode
 *
 * The `_alive` flags say how far bring-up got, so teardown unwinds exactly
 * what was set up and in the reverse order.
 */
struct xnvme_be_upcie_rte_mem {
	struct hostmem_config config;
	struct hostmem_hugepage hp;
	struct dmamem dmem;
	struct dmamem_heap heap;
	int hp_alive;   ///< Whether hp holds an allocation
	int dmem_alive; ///< Whether dmem wraps hp
	int heap_alive; ///< Whether heap is initialized
};

/**
 * This process's view of a runtime another process owns
 *
 * Empty in the process that owns one. `sock` is what the server watches: it
 * closing is how a client's queues and allocs come back.
 */
struct xnvme_be_upcie_rte_attached {
	int sock;                                 ///< To the server; -1 when not attached
	int alive;                                ///< Whether the rest of this means anything
	void *heap_base;                          ///< This process's mapping of the heap
	uint64_t heap_nbytes;                     ///< How much of it
	void *bar0;                               ///< This process's mapping of BAR0
	uint64_t bar0_nbytes;                     ///< How much of it
	const struct nvme_runtime_record *record; ///< In the heap, written by the server
};

struct xnvme_be_upcie_rte {
	enum xnvme_be_upcie_mode mode;
	struct xnvme_be_upcie_rte_cdev cdev;
	struct xnvme_be_upcie_rte_type1 type1;
	struct xnvme_be_upcie_rte_mem mem;
	struct xnvme_be_upcie_rte_attached attached; ///< Set when another process owns this
	int is_initialized;
};

extern struct xnvme_be_upcie_rte g_upcie_rte;

extern struct xnvme_be_mem g_xnvme_be_upcie_mem;
extern struct xnvme_be_admin g_xnvme_be_upcie_admin;
extern struct xnvme_be_sync g_xnvme_be_upcie_sync;
extern struct xnvme_be_async g_xnvme_be_upcie_async;
extern struct xnvme_be_dev g_xnvme_be_upcie_dev;

// Attachment: driver probing, mode selection and vfio wiring (xnvme_be_upcie_vfio.c)
int
xnvme_be_upcie_get_driver_name(const char *bdf, char *driver_name, size_t driver_name_len);

int
xnvme_be_upcie_resolve_vfio_cdev(const char *bdf, char *cdev_path, size_t cdev_path_len);

int
xnvme_be_upcie_mode_from_driver(const char *bdf, const char *driver_name,
				enum xnvme_be_upcie_mode *mode, char *cdev_path,
				size_t cdev_path_len);

int
xnvme_be_upcie_type1_attach(struct xnvme_be_upcie_ctrlr *ctrlr, const char *bdf);

// Runtime and device lifecycle (xnvme_be_upcie_dev.c)
/**
 * Address-space width the DMA-address table is sized for; 0 for the default.
 *
 * Read from XNVME_UPCIE_VA_BITS. See the definition for what it costs and when
 * lowering it is warranted.
 */
int
xnvme_be_upcie_va_bits(void);

void
xnvme_be_upcie_dev_close(struct xnvme_dev *dev);
int
xnvme_be_upcie_dev_open(struct xnvme_dev *dev);
void *
xnvme_be_upcie_ctrlr_init(struct xnvme_dev *dev);
int
xnvme_be_upcie_ctrlr_term(void *handle);

/**
 * The admin, sync and async paths, shared by `upcie`, `upcie-cuda` and `upcie-hip`
 *
 * They translate payloads through the per-device dmamem in
 * xnvme_be_upcie_state, so the same implementation serves host and device
 * memory. The GPU backends wire these into their own xnvme_be_{admin,sync,
 * async} in xnvme_be_upcie_{cuda,hip}.c.
 */
int
xnvme_be_upcie_dmamem_map(struct dmamem *dmem, void *vaddr, size_t nbytes, uint64_t *phys);

int
xnvme_be_upcie_dmamem_unmap(struct dmamem *dmem, void *vaddr);

int
xnvme_be_upcie_queue_init(struct xnvme_queue *queue, int opts);
int
xnvme_be_upcie_queue_term(struct xnvme_queue *queue);
int
xnvme_be_upcie_queue_poke(struct xnvme_queue *queue, uint32_t max);
int
xnvme_be_upcie_async_cmd_io(struct xnvme_cmd_ctx *ctx, void *dbuf, size_t dbuf_nbytes, void *mbuf,
			    size_t mbuf_nbytes);
int
xnvme_be_upcie_async_cmd_iov(struct xnvme_cmd_ctx *ctx, struct iovec *dvec, size_t dvec_cnt,
			     size_t dvec_nbytes, void *mbuf, size_t mbuf_nbytes);
int
xnvme_be_upcie_sync_cmd_io(struct xnvme_cmd_ctx *ctx, void *dbuf, size_t dbuf_nbytes, void *mbuf,
			   size_t mbuf_nbytes);
int
xnvme_be_upcie_sync_cmd_iov(struct xnvme_cmd_ctx *ctx, struct iovec *dvec, size_t dvec_cnt,
			    size_t dvec_nbytes, void *mbuf, size_t mbuf_nbytes);
int
xnvme_be_upcie_sync_cmd_admin(struct xnvme_cmd_ctx *ctx, void *dbuf, size_t dbuf_nbytes,
			      void *mbuf, size_t mbuf_nbytes);
int
xnvme_be_upcie_sync_cmd_pseudo(struct xnvme_cmd_ctx *ctx, void *dbuf, size_t dbuf_nbytes,
			       void *mbuf, size_t mbuf_nbytes);

// DMA buffers, from the heap this process owns or the one it attached to
// (xnvme_be_upcie_mem.c)

void *
xnvme_be_upcie_buf_alloc(const struct xnvme_dev *dev, size_t nbytes, uint64_t *phys);

void
xnvme_be_upcie_buf_free(const struct xnvme_dev *dev, void *buf);

#endif /* __INTERNAL_XNVME_BE_UPCIE */
