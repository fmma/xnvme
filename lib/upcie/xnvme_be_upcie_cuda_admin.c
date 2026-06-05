// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#include <xnvme_be.h>
#include <xnvme_be_nosys.h>
#ifdef XNVME_BE_UPCIE_CUDA_ENABLED
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <xnvme_dev.h>
#include <xnvme_queue.h>
#include <xnvme_be_upcie_cuda.h>

/**
 * Serve admin commands in attach mode on the cuda backend.
 *
 * Mirrors xnvme_be_upcie_attach_cmd_admin but writes the canned Identify
 * payload into the (GPU) data buffer with cuMemcpyHtoD, since xnvme_buf_alloc
 * on upcie-cuda returns device memory a host memcpy cannot touch.
 */
static int
_attach_cmd_admin_cuda(struct xnvme_cmd_ctx *ctx, void *dbuf, size_t dbuf_nbytes)
{
	struct xnvme_be_upcie_state *state = (void *)ctx->dev->be.state;
	struct upcie_attach_desc *adesc = state->ctrlr->adesc;
	uint8_t opc = ctx->cmd.common.opcode;
	uint8_t staging[UPCIE_ATTACH_IDFY_NBYTES];
	const void *src = NULL;
	uint8_t cns;

	memset(&ctx->cpl, 0, sizeof(ctx->cpl));

	if (opc != XNVME_SPEC_ADM_OPC_IDFY) {
		if (dbuf && dbuf_nbytes) {
			cuMemsetD8((CUdeviceptr)(uintptr_t)dbuf, 0, dbuf_nbytes);
		}
		return 0;
	}
	if (!dbuf || dbuf_nbytes < UPCIE_ATTACH_IDFY_NBYTES) {
		return -EINVAL;
	}

	cns = (uint8_t)ctx->cmd.idfy.cns;
	switch (cns) {
	case XNVME_SPEC_IDFY_NS:
		src = adesc->idfy_ns;
		break;
	case XNVME_SPEC_IDFY_CTRLR:
		src = adesc->idfy_ctrlr;
		break;
	case 0x02: ///< Active Namespace ID list
		memset(staging, 0, sizeof(staging));
		((uint32_t *)staging)[0] = adesc->nsid;
		src = staging;
		break;
	default:
		/* Command-set-specific (ZONED/FS) and other identifies are not
		 * synthesised. Report a command error like a conventional
		 * controller so geometry derivation skips its direct read of the
		 * (device-memory) response buffer. */
		ctx->cpl.status.sc = 0x02; ///< Invalid Field in Command
		return 0;
	}

	if (cuMemcpyHtoD((CUdeviceptr)(uintptr_t)dbuf, src, UPCIE_ATTACH_IDFY_NBYTES) !=
	    CUDA_SUCCESS) {
		XNVME_DEBUG("FAILED: cuMemcpyHtoD(identify)");
		return -EIO;
	}
	return 0;
}

int
xnvme_be_upcie_cuda_sync_cmd_admin(struct xnvme_cmd_ctx *ctx, void *dbuf, size_t dbuf_nbytes,
				   void *XNVME_UNUSED(mbuf), size_t XNVME_UNUSED(mbuf_nbytes))
{
	struct xnvme_be_upcie_state *state = (void *)ctx->dev->be.state;
	struct nvme_controller *ctrlr = state->ctrlr->ctrl;
	struct nvme_command *cmd = (struct nvme_command *)&ctx->cmd;
	struct nvme_completion *cpl = (struct nvme_completion *)&ctx->cpl;
	struct nvme_request *req;
	int err;

	if (state->ctrlr->attach) {
		return _attach_cmd_admin_cuda(ctx, dbuf, dbuf_nbytes);
	}

	req = nvme_request_alloc(ctrlr->aq.rpool);
	if (!req) {
		XNVME_DEBUG("FAILED: nvme_request_alloc(); errno(%d)", errno);
		return -errno;
	}

	req->user = ctx;
	cmd->cid = req->cid;

	if (dbuf) {
		if (cudamem_heap_contains(&g_upcie_cuda_rte.cuda_heap, dbuf)) {
			nvme_request_prep_command_prps_contig_cuda(req, &g_upcie_cuda_rte.cuda_heap,
								   dbuf, dbuf_nbytes, cmd);
		} else {
			err = nvme_request_prep_command_prps_contig_cuda_mapped(
				req, &g_upcie_cuda_rte.mappings,
				g_upcie_cuda_rte.cuda_heap.config, dbuf, dbuf_nbytes, cmd);
			if (err) {
				XNVME_DEBUG("FAILED: prps_contig_cuda_mapped(); err(%d)", err);
				goto exit;
			}
		}
	}

	err = nvme_qpair_enqueue(&ctrlr->aq, cmd);
	if (err) {
		XNVME_DEBUG("FAILED: nvme_qpair_enqueue();");
		goto exit;
	}

	nvme_qpair_sqdb_update(&ctrlr->aq);

	err = nvme_qpair_reap_cpl(&ctrlr->aq, ctrlr->timeout_ms, cpl);
	if (err) {
		XNVME_DEBUG("FAILED: nvme_qpair_reap_cpl();");
		goto exit;
	}

	if (xnvme_cmd_ctx_cpl_status(ctx)) {
		XNVME_DEBUG("FAILED: command; sc(%d); sct(%d)", ctx->cpl.status.sc,
			    ctx->cpl.status.sct);
		err = -EIO;
	}

exit:
	nvme_request_free(ctrlr->aq.rpool, req->cid);
	return err;
}
#endif

struct xnvme_be_admin g_xnvme_be_upcie_cuda_admin = {
	.id = "upcie-cuda",
#ifdef XNVME_BE_UPCIE_CUDA_ENABLED
	.cmd_admin = xnvme_be_upcie_cuda_sync_cmd_admin,
	.cmd_pseudo = xnvme_be_nosys_sync_cmd_pseudo,
#else
	.cmd_admin = xnvme_be_nosys_sync_cmd_admin,
	.cmd_pseudo = xnvme_be_nosys_sync_cmd_pseudo,
#endif
};
