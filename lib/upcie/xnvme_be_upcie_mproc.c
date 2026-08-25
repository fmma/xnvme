// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

/**
 * Reporting on a runtime without joining it
 *
 * Everything here asks whoever is serving an identifier and takes the answer,
 * rather than reading a shared segment and inferring one. Connecting is most of
 * the answer: a process that answers is a process holding the controllers.
 *
 * A previous arrangement kept this in shared memory, where a killed server
 * left a segment that still read as plausible and had to be told apart from a
 * live one. Nothing here has that problem: a socket that answers has somebody
 * behind it.
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <libxnvme.h>
#include <libxnvme_mproc.h>
#include <xnvme_be.h>
#include <xnvme_dev.h>

#ifdef XNVME_BE_UPCIE_ENABLED
#include <xnvme_be_upcie.h>

int
xnvme_mproc_primary_alive(uint32_t shm_id)
{
	struct nvme_cplane_msg msg = {0};
	int err = xnvme_be_upcie_query(shm_id, NULL, &msg);

	if (!err) {
		return 1;
	}

	return (err == -ENOENT) ? 0 : err;
}

int
xnvme_mproc_get_info(uint32_t shm_id, struct xnvme_mproc_info *info)
{
	struct nvme_cplane_msg msg = {0};
	int err;

	if (!info) {
		return -EINVAL;
	}

	err = xnvme_be_upcie_query(shm_id, NULL, &msg);
	if (err) {
		return err;
	}

	memset(info, 0, sizeof(*info));

	/* The count is of clients; whoever answered is one more. */
	info->nattached = msg.u.status.nconsumers + 1;

	if (msg.u.status.bdf[0]) {
		info->nctrlrs = 1;
		info->nctrlrs_held = 1;
		snprintf(info->ctrlrs[0], sizeof(info->ctrlrs[0]), "%s", msg.u.status.bdf);
	}

	return 0;
}

int
xnvme_mproc_get_ctrlr_info(const char *uri, struct xnvme_mproc_ctrlr_info *info)
{
	struct dirent *entry;
	DIR *dir;

	if (!uri || !info) {
		return -EINVAL;
	}

	/* The caller names a controller, not a runtime, so the runtimes are
	 * asked in turn until one says it holds that controller. */
	dir = opendir("/tmp");
	if (!dir) {
		return -errno;
	}

	while ((entry = readdir(dir))) {
		struct nvme_cplane_msg msg = {0};
		unsigned int shm_id;

		if (sscanf(entry->d_name, "xnvme-homi-%u.sock", &shm_id) != 1) {
			continue;
		}
		if (xnvme_be_upcie_query((uint32_t)shm_id, NULL, &msg)) {
			continue;
		}
		if (strcmp(msg.u.status.bdf, uri)) {
			continue;
		}

		closedir(dir);

		memset(info, 0, sizeof(*info));
		info->nattached = msg.u.status.nconsumers + 1;
		info->nsq_used = msg.u.status.nqueues;
		info->ncq_used = msg.u.status.nqueues;
		info->initialized = 1;

		return 0;
	}

	closedir(dir);

	return -ENOENT;
}
#else
int
xnvme_mproc_primary_alive(uint32_t XNVME_UNUSED(shm_id))
{
	return -ENOSYS;
}

int
xnvme_mproc_get_info(uint32_t XNVME_UNUSED(shm_id), struct xnvme_mproc_info *XNVME_UNUSED(info))
{
	return -ENOSYS;
}

int
xnvme_mproc_get_ctrlr_info(const char *XNVME_UNUSED(uri),
			   struct xnvme_mproc_ctrlr_info *XNVME_UNUSED(info))
{
	return -ENOSYS;
}
#endif
