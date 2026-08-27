// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

/**
 * Serving clients of a runtime this process owns
 *
 * The loop is small because the pieces are elsewhere: uPCIe defines what
 * passes between the two, and the backend knows how to describe a runtime and
 * how to create a queue. What is left here is the bookkeeping nobody else can
 * do, which is remembering what each client holds so that a disconnect can
 * release it.
 *
 * A disconnect is the only reliable signal there is. A client that exits
 * cleanly hands its queues back; one that is killed mid-command does not, and
 * the socket closing is what says so.
 */
#include <errno.h>

#include <libxnvme.h>
#include <libxnvme_mproc.h>
#include <xnvme_be.h>
#include <xnvme_dev.h>

#ifdef XNVME_BE_UPCIE_ENABLED
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <xnvme_be_upcie.h>

#define SERVE_CLIENTS_MAX 32

/**
 * Controllers one server will serve
 *
 * One socket and one export per controller, since the control plane protocol has
 * nowhere to name a controller and the identifier alone cannot say which one a
 * client wants.
 */
#define SERVE_DEVS_MAX 16
#define SERVE_IOQPAIRS_PER_CLIENT 8

#define SERVE_ALLOCS_PER_CLIENT 1024

struct serve_client {
	int sock;
	int dev; ///< Index into the server's device list; what this client attached to
	uint32_t qids[SERVE_IOQPAIRS_PER_CLIENT];
	int nqids;
	uint64_t allocs[SERVE_ALLOCS_PER_CLIENT]; ///< Heap offsets handed out
	int nallocs;
};

/* What a status request reports. Counted rather than derived, because the
 * answer is wanted while the loop is between polls. */
static int serve_nclients;
/**
 * Queues handed out, per controller
 *
 * Per controller rather than per process: a server serving several would
 * otherwise report the same number against each of them, which reads as every
 * controller being as busy as the busiest.
 */
static int serve_nqueues[SERVE_DEVS_MAX];

/**
 * Release everything a client held, queues before the memory behind them
 */
/**
 * Where a client of one particular controller connects
 *
 * Derived from the runtime-wide path rather than rebuilt from the identifier,
 * so the two cannot drift: the client side builds the same name in
 * xnvme_be_upcie_socket_path().
 */
static void
serve_dev_path(const char *base, const char *bdf, char *path, size_t nbytes)
{
	char stem[192] = {0};
	char sane[32] = {0};
	size_t len;

	snprintf(stem, sizeof(stem), "%s", base);
	len = strlen(stem);
	if ((len > 5) && !strcmp(stem + len - 5, ".sock")) {
		stem[len - 5] = '\0';
	}

	snprintf(sane, sizeof(sane), "%s", bdf);
	for (size_t i = 0; sane[i]; ++i) {
		if ((sane[i] == ':') || (sane[i] == '.') || (sane[i] == '/')) {
			sane[i] = '-';
		}
	}

	snprintf(path, nbytes, "%s-%s.sock", stem, sane);
}

static void
serve_client_release(struct xnvme_dev *dev, struct serve_client *client)
{
	/* Queues first: the controller has to stop being able to reach an
	 * address before that address stops meaning anything. */
	for (int i = 0; i < client->nqids; ++i) {
		int err = xnvme_be_upcie_free_ioqpair(dev, client->qids[i]);

		if (err) {
			XNVME_DEBUG("FAILED: ungrant(qid(%u)); err(%d)", client->qids[i], err);
		}
	}

	for (int i = 0; i < client->nallocs; ++i) {
		int err = xnvme_be_upcie_free_buf(client->allocs[i]);

		if (err) {
			XNVME_DEBUG("FAILED: reclaim(0x%" PRIx64 "); err(%d)", client->allocs[i],
				    err);
		}
	}

	serve_nqueues[client->dev] -= client->nqids;

	if (client->sock >= 0) {
		close(client->sock);
		serve_nclients--;
	}

	memset(client, 0, sizeof(*client));
	client->sock = -1;
}

/**
 * Answer one message, and record what the answer handed out
 */
static int
serve_one(struct xnvme_dev *dev, struct serve_client *client,
	  const struct xnvme_be_upcie_export *exported)
{
	struct nvme_cplane_msg msg = {0};
	struct nvme_cplane_msg reply = {0};
	int fds[NVME_CPLANE_FDS_MAX];
	uint32_t nfds = 0;
	int err;

	err = nvme_cplane_msg_recv(client->sock, &msg, NULL, NULL);
	if (err) {
		return err; ///< -ENOTCONN when the client is gone
	}

	reply.op = msg.op;
	reply.version = NVME_CPLANE_VERSION;

	if (msg.version != NVME_CPLANE_VERSION) {
		XNVME_DEBUG("FAILED: client speaks version(%u)", msg.version);
		reply.status = -EPROTO;
		return nvme_cplane_msg_send(client->sock, &reply, NULL, 0);
	}

	switch (msg.op) {
	case NVME_CPLANE_OP_ATTACH:
		reply.u.attach.record_offset = exported->record_offset;
		reply.u.attach.heap_nbytes = exported->heap_nbytes;
		reply.u.attach.bar0_nbytes = exported->bar0_nbytes;
		fds[nfds++] = exported->heap_fd;
		fds[nfds++] = exported->bar0_fd;
		break;

	case NVME_CPLANE_OP_ALLOC_IOQPAIR: {
		struct xnvme_be_upcie_qgrant allocation = {0};

		if (client->nqids == SERVE_IOQPAIRS_PER_CLIENT) {
			reply.status = -ENOSPC;
			break;
		}

		reply.status = xnvme_be_upcie_alloc_ioqpair(dev, msg.u.queue.depth, &allocation);
		if (reply.status) {
			break;
		}

		client->qids[client->nqids++] = allocation.qid;
		serve_nqueues[client->dev]++;

		reply.u.queue.allocation.sq_offset = allocation.sq_offset;
		reply.u.queue.allocation.cq_offset = allocation.cq_offset;
		reply.u.queue.allocation.prp_offset = allocation.prp_offset;
		reply.u.queue.allocation.qid = allocation.qid;
		reply.u.queue.allocation.depth = allocation.depth;
	} break;

	case NVME_CPLANE_OP_FREE_IOQPAIR:
		reply.status = -ENOENT;
		for (int i = 0; i < client->nqids; ++i) {
			if (client->qids[i] != msg.u.release.qid) {
				continue;
			}

			reply.status = xnvme_be_upcie_free_ioqpair(dev, msg.u.release.qid);
			client->qids[i] = client->qids[--client->nqids];
			serve_nqueues[client->dev]--;
			break;
		}
		break;

	case NVME_CPLANE_OP_ALLOC_BUF: {
		uint64_t offset = 0;

		if (client->nallocs == SERVE_ALLOCS_PER_CLIENT) {
			reply.status = -ENOSPC;
			break;
		}

		reply.status = xnvme_be_upcie_alloc_buf(msg.u.mem.nbytes, &offset);
		if (reply.status) {
			break;
		}

		client->allocs[client->nallocs++] = offset;
		reply.u.mem.offset = offset;
	} break;

	case NVME_CPLANE_OP_FREE_BUF:
		reply.status = -ENOENT;
		for (int i = 0; i < client->nallocs; ++i) {
			if (client->allocs[i] != msg.u.mem.offset) {
				continue;
			}

			reply.status = xnvme_be_upcie_free_buf(msg.u.mem.offset);
			client->allocs[i] = client->allocs[--client->nallocs];
			break;
		}
		break;

	case NVME_CPLANE_OP_STATUS:
		/* Asking is not attaching, so whoever is asking does not count
		 * itself among the clients. */
		reply.u.status.nconsumers = (uint32_t)(serve_nclients - 1);
		reply.u.status.nqueues = (uint32_t)serve_nqueues[client->dev];
		snprintf(reply.u.status.bdf, sizeof(reply.u.status.bdf), "%s", exported->uri);
		break;

	case NVME_CPLANE_OP_ADMIN_CMD:
		if (!nvme_cplane_admin_permitted(&msg.u.admin.cmd)) {
			reply.status = -EPERM;
			break;
		}
		reply.status = xnvme_be_upcie_admin(dev, &msg.u.admin.cmd, &reply.u.admin.cpl);
		break;

	default:
		reply.status = -ENOSYS;
		break;
	}

	reply.nfds = nfds;

	return nvme_cplane_msg_send(client->sock, &reply, nfds ? fds : NULL, nfds);
}

int
xnvme_mproc_serve(struct xnvme_dev **devs, int ndevs, const char *path,
		  volatile sig_atomic_t *stop)
{
	struct xnvme_be_upcie_export exported[SERVE_DEVS_MAX] = {0};
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	struct serve_client clients[SERVE_CLIENTS_MAX];
	struct pollfd pfds[SERVE_CLIENTS_MAX + SERVE_DEVS_MAX + 1];
	char paths[SERVE_DEVS_MAX + 1][256] = {{0}};
	int listeners[SERVE_DEVS_MAX + 1];
	int listener_dev[SERVE_DEVS_MAX + 1];
	int nlisteners = 0;
	int nexported = 0;
	int err;

	if (!devs || (ndevs < 1) || !path || !stop) {
		return -EINVAL;
	}
	if (ndevs > SERVE_DEVS_MAX) {
		/* Distinct from the -ENOSYS below: that one says the backend
		 * shares its own way and the caller should just hold the
		 * devices. Holding them here would leave clients to open the
		 * controllers themselves, which closes them out from under the
		 * server. */
		XNVME_DEBUG("FAILED: serving %d devices; the cap is %d", ndevs, SERVE_DEVS_MAX);
		return -EOPNOTSUPP;
	}

	/* Every field read below belongs to this backend's own state, so a
	 * device opened through another one is refused rather than
	 * reinterpreted. Callers treat -ENOSYS as "hold the devices, serve
	 * nobody", which is what a backend with its own sharing wants. */
	if (strcmp(devs[0]->be.attr.name, "upcie")) {
		XNVME_DEBUG("FAILED: serving is uPCIe's; be(%s) has its own arrangement",
			    devs[0]->be.attr.name);
		return -ENOSYS;
	}

	for (nexported = 0; nexported < ndevs; ++nexported) {
		err = xnvme_be_upcie_export(devs[nexported], &exported[nexported]);
		if (err) {
			XNVME_DEBUG("FAILED: xnvme_be_upcie_export(%d); err(%d)", nexported, err);
			goto exit;
		}
	}

	/* Every field, not the ones that came to mind: a stack array is not
	 * zeroed, and a count read as garbage passes a bounds check that then
	 * writes wherever it likes. */
	memset(clients, 0, sizeof(clients));
	for (int i = 0; i < SERVE_CLIENTS_MAX; ++i) {
		clients[i].sock = -1;
	}

	snprintf(paths[0], sizeof(paths[0]), "%s", path);
	listener_dev[0] = 0;
	for (int i = 0; i < ndevs; ++i) {
		serve_dev_path(path, devs[i]->ident.uri, paths[i + 1], sizeof(paths[i + 1]));
		listener_dev[i + 1] = i;
	}

	for (nlisteners = 0; nlisteners < (ndevs + 1); ++nlisteners) {
		unlink(paths[nlisteners]);

		/* Non-blocking, because accept() is drained in a loop: on a
		 * blocking listener the call after the last waiting connection
		 * waits for one that is not coming, and the loop never gets
		 * back to poll(). */
		listeners[nlisteners] = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
		if (listeners[nlisteners] < 0) {
			err = -errno;
			goto exit;
		}

		memset(&addr, 0, sizeof(addr));
		addr.sun_family = AF_UNIX;
		snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", paths[nlisteners]);

		/* Deep enough to absorb a burst of probes. Something asking
		 * whether a runtime is alive gets its answer from connecting,
		 * so a refused connection reads as a runtime that is not
		 * there. */
		if (bind(listeners[nlisteners], (struct sockaddr *)&addr, sizeof(addr)) ||
		    listen(listeners[nlisteners], SERVE_CLIENTS_MAX * 8)) {
			err = -errno;
			close(listeners[nlisteners]);
			goto exit;
		}
	}

	while (!*stop) {
		int nfds = 0;

		for (int i = 0; i < nlisteners; ++i) {
			pfds[nfds].fd = listeners[i];
			pfds[nfds].events = POLLIN;
			nfds++;
		}

		for (int i = 0; i < SERVE_CLIENTS_MAX; ++i) {
			if (clients[i].sock < 0) {
				continue;
			}
			pfds[nfds].fd = clients[i].sock;
			pfds[nfds].events = POLLIN;
			nfds++;
		}

		/* A timeout rather than a signal mask: the caller's flag is
		 * what ends this, and a second of latency on the way out costs
		 * nobody anything. */
		if (poll(pfds, nfds, 1000) < 0) {
			if (errno == EINTR) {
				continue;
			}
			err = -errno;
			break;
		}

		for (int l = 0; l < nlisteners; ++l) {
			while (pfds[l].revents & POLLIN) {
				/* Accepted blocking on purpose: reading a
				 * message waits for the rest of one, and a
				 * client that pauses before writing has not
				 * gone away. */
				int sock = accept4(listeners[l], NULL, NULL, 0);
				int slot;

				if (sock < 0) {
					break; ///< Drained, or nothing was waiting
				}

				for (slot = 0; slot < SERVE_CLIENTS_MAX; ++slot) {
					if (clients[slot].sock < 0) {
						break;
					}
				}
				if (slot == SERVE_CLIENTS_MAX) {
					close(sock);
					continue;
				}

				clients[slot].sock = sock;
				clients[slot].dev = listener_dev[l];
				serve_nclients++;
			}
		}

		for (int i = 0; i < SERVE_CLIENTS_MAX; ++i) {
			if (clients[i].sock < 0) {
				continue;
			}

			for (int p = nlisteners; p < nfds; ++p) {
				struct xnvme_dev *dev = devs[clients[i].dev];

				if ((pfds[p].fd != clients[i].sock) || !pfds[p].revents) {
					continue;
				}

				if (serve_one(dev, &clients[i], &exported[clients[i].dev])) {
					serve_client_release(dev, &clients[i]);
				}
				break;
			}
		}
	}

	for (int i = 0; i < SERVE_CLIENTS_MAX; ++i) {
		if (clients[i].sock >= 0) {
			serve_client_release(devs[clients[i].dev], &clients[i]);
		}
	}

exit:
	for (int i = 0; i < nlisteners; ++i) {
		close(listeners[i]);
		unlink(paths[i]);
	}
	for (int i = 0; i < nexported; ++i) {
		xnvme_be_upcie_unexport(&exported[i]);
	}

	return err;
}
#else
int
xnvme_mproc_serve(struct xnvme_dev **XNVME_UNUSED(devs), int XNVME_UNUSED(ndevs),
		  const char *XNVME_UNUSED(path), volatile sig_atomic_t *XNVME_UNUSED(stop))
{
	return -ENOSYS;
}
#endif
