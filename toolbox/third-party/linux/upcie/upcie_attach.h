// SPDX-License-Identifier: BSD-3-Clause

/**
 * Cross-process NVMe controller-attach descriptor
 * ===============================================
 *
 * Wire format placed by a controller owner into a file, and read by a client
 * that wants to drive the controller's I/O queues without owning the
 * controller. It bundles everything a client needs to attach:
 *
 *   - bdf / region_path: where to map BAR0 (doorbells) and the shared
 *     hugepage region holding the SQ/CQ rings.
 *   - idfy_ctrlr / idfy_ns: the raw 4 KiB NVMe Identify Controller and
 *     Identify Namespace payloads the owner already fetched on the admin
 *     queue, so a client can derive geometry without an admin queue.
 *   - qpairs[]: one nvme_qpair_export per I/O queue the owner pre-created and
 *     is handing out; a client imports these with nvme_qpair_import().
 *
 * The format carries no transport assumptions: how the owner and client agree
 * on the file path is out of scope. Requires the NVMe portion of upcie (define
 * _UPCIE_WITH_NVME before including <upcie/upcie.h>).
 *
 * @file upcie_attach.h
 */
#ifndef UPCIE_ATTACH_H
#define UPCIE_ATTACH_H

#include <stdio.h>

#define UPCIE_ATTACH_MAX_QPAIRS 16
#define UPCIE_ATTACH_IDFY_NBYTES 4096

struct upcie_attach_desc {
	char bdf[32];          ///< PCI BDF of the controller, e.g. "0000:01:00.0"
	char region_path[256]; ///< Path to the shared hugepage holding the rings
	uint64_t region_size;  ///< Size of the shared region in bytes
	uint32_t nsid;         ///< Namespace identifier the geometry describes
	uint32_t nqpairs;      ///< Number of valid entries in qpairs[]
	uint8_t idfy_ctrlr[UPCIE_ATTACH_IDFY_NBYTES]; ///< Identify Controller payload
	uint8_t idfy_ns[UPCIE_ATTACH_IDFY_NBYTES];    ///< Identify Namespace payload
	struct nvme_qpair_export qpairs[UPCIE_ATTACH_MAX_QPAIRS];
};

/**
 * Write an attach descriptor to `path`.
 *
 * @return 0 on success, negative errno on failure.
 */
static inline int
upcie_attach_write(const char *path, const struct upcie_attach_desc *desc)
{
	FILE *f;

	f = fopen(path, "wb");
	if (!f) {
		return -errno;
	}
	if (fwrite(desc, sizeof(*desc), 1, f) != 1) {
		fclose(f);
		return -EIO;
	}
	if (fclose(f) != 0) {
		return -errno;
	}

	return 0;
}

/**
 * Read an attach descriptor from `path`.
 *
 * @return 0 on success, negative errno on failure.
 */
static inline int
upcie_attach_read(const char *path, struct upcie_attach_desc *desc)
{
	FILE *f;

	f = fopen(path, "rb");
	if (!f) {
		return -errno;
	}
	if (fread(desc, sizeof(*desc), 1, f) != 1) {
		fclose(f);
		return -EIO;
	}
	fclose(f);

	return 0;
}

#endif /* UPCIE_ATTACH_H */
