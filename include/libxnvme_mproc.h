// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

/**
 * Sharing a controller with other processes
 *
 * One process opens a controller and serves it; others attach and do I/O
 * against the same device. The server hands over descriptors rather than naming
 * objects in the filesystem, because a vfio device file cannot be bound twice,
 * so what a client needs cannot be found by name.
 *
 * @note uPCIe only, so -ENOSYS elsewhere.
 *
 * @note This API is experimental and may change without notice.
 *
 * @file libxnvme_mproc.h
 */

#ifndef __LIBXNVME_MPROC_H
#define __LIBXNVME_MPROC_H

#include <signal.h>
#include <stdint.h>

/**
 * Serve clients of the given devices until told to stop
 *
 * Holds a socket at `path` and answers processes that attach to it: hands over
 * the descriptors and offsets they need to build their own view of the
 * runtime, creates queues on request, submits admin commands on their behalf,
 * and releases whatever a client held when it disconnects.
 *
 * Blocks until `*stop` becomes non-zero, which a signal handler is expected to
 * do. Returns when the last thing it was serving has been let go, so a caller
 * can close its devices afterwards.
 *
 * @param devs Devices this process has opened
 * @param ndevs How many
 * @param path Where to hold the socket; a filesystem path, since an
 * abstract-namespace address cannot be reached from another network namespace
 * @param stop Set to non-zero to bring the loop down; typed for a signal
 * handler, since that is what usually sets it
 *
 * @return 0 on success, negative errno on error
 */
int
xnvme_mproc_serve(struct xnvme_dev **devs, int ndevs, const char *path,
		  volatile sig_atomic_t *stop);

#endif /* __LIBXNVME_MPROC_H */
