// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#include <libxnvme.h>
#include <xnvme_be.h>
#include <xnvme_be_nosys.h>
#ifdef XNVME_BE_UPCIE_ENABLED
#include <xnvme_dev.h>
#include <xnvme_queue.h>
#include <xnvme_spec.h>
#include <xnvme_be_upcie.h>

/**
 * Serve admin commands in attach mode, where there is no admin queue.
 *
 * Only Identify is meaningfully supported: the Controller/Namespace payloads
 * the owner captured are returned from the descriptor so geometry derivation
 * succeeds. The active-namespace list is synthesised from the single namespace
 * the descriptor describes. Other admin commands return a zeroed buffer and
 * success so geometry derivation does not hard-fail on optional probes.
 */
static int
_attach_cmd_admin(struct xnvme_cmd_ctx *ctx, void *dbuf, size_t dbuf_nbytes)
{
	struct xnvme_be_upcie_state *state = (void *)ctx->dev->be.state;
	struct upcie_attach_desc *adesc = state->ctrlr->adesc;
	uint8_t opc = ctx->cmd.common.opcode;
	uint8_t cns;

	memset(&ctx->cpl, 0, sizeof(ctx->cpl));

	if (opc != XNVME_SPEC_ADM_OPC_IDFY) {
		if (dbuf && dbuf_nbytes) {
			state->dbuf_write(dbuf, 0, NULL, dbuf_nbytes);
		}
		return 0;
	}

	if (!dbuf || dbuf_nbytes < UPCIE_ATTACH_IDFY_NBYTES) {
		XNVME_DEBUG("FAILED: attach idfy dbuf too small (%zu)", dbuf_nbytes);
		return -EINVAL;
	}

	cns = (uint8_t)ctx->cmd.idfy.cns;
	switch (cns) {
	case XNVME_SPEC_IDFY_NS:
		return state->dbuf_write(dbuf, 0, adesc->idfy_ns, UPCIE_ATTACH_IDFY_NBYTES);
	case XNVME_SPEC_IDFY_CTRLR:
		return state->dbuf_write(dbuf, 0, adesc->idfy_ctrlr, UPCIE_ATTACH_IDFY_NBYTES);
	case 0x02: ///< Active Namespace ID list
		state->dbuf_write(dbuf, 0, NULL, dbuf_nbytes);
		return state->dbuf_write(dbuf, 0, &adesc->nsid, sizeof(adesc->nsid));
	default:
		/* Command-set-specific (ZONED/FS) and other identifies are not
		 * synthesised. Report a command error like a conventional
		 * controller so geometry derivation does not treat a zeroed
		 * buffer as a valid response and read it. */
		ctx->cpl.status.sc = 0x02; ///< Invalid Field in Command
		return 0;
	}
}

int
xnvme_be_upcie_sync_cmd_admin(struct xnvme_cmd_ctx *ctx, void *dbuf, size_t dbuf_nbytes,
			      void *XNVME_UNUSED(mbuf), size_t XNVME_UNUSED(mbuf_nbytes))
{
	struct xnvme_be_upcie_state *state = (void *)ctx->dev->be.state;
	struct xnvme_be_upcie_ctrlr *ctrlr = state->ctrlr;
	struct nvme_controller *ctrl = state->ctrlr->ctrl;
	struct nvme_command *cmd = (struct nvme_command *)&ctx->cmd;
	struct nvme_completion *cpl = (struct nvme_completion *)&ctx->cpl;
	struct nvme_request *req;
	int err;

	if (ctrlr->handout) {
		return _attach_cmd_admin(ctx, dbuf, dbuf_nbytes);
	}

	err = xnvme_be_upcie_ctrlr_mutex_lock(ctrlr);
	if (err) {
		XNVME_DEBUG("FAILED: xnvme_be_upcie_ctrlr_mutex_lock(); err(%d)", err);
		return err;
	}

	req = nvme_request_alloc(ctrl->aq.rpool);
	if (!req) {
		XNVME_DEBUG("FAILED: nvme_request_alloc(); errno(%d)", errno);
		err = -errno;
		xnvme_be_upcie_ctrlr_mutex_unlock(ctrlr);
		return err;
	}

	req->user = ctx;
	cmd->cid = req->cid;

	if (dbuf) {
		err = nvme_request_prep_command_prps_contig_dmamem(req, state->dmem, dbuf,
								   dbuf_nbytes, cmd);
		if (err) {
			XNVME_DEBUG("FAILED: prps_contig_dmamem(); err(%d)", err);
			goto exit;
		}
	}

	err = nvme_qpair_enqueue(&ctrl->aq, cmd);
	if (err) {
		XNVME_DEBUG("FAILED: nvme_qpair_enqueue(); err(%d)", err);
		goto exit;
	}

	nvme_qpair_sqdb_update(&ctrl->aq);

	err = nvme_qpair_reap_cpl(&ctrl->aq, ctrl->timeout_ms, cpl);
	if (err) {
		XNVME_DEBUG("FAILED: nvme_qpair_reap_cpl(); err(%d)", err);
		goto exit;
	}

	if (xnvme_cmd_ctx_cpl_status(ctx)) {
		XNVME_DEBUG("FAILED: command; sc(%d); sct(%d)", ctx->cpl.status.sc,
			    ctx->cpl.status.sct);
		err = -EIO;
	}

exit:
	xnvme_be_upcie_ctrlr_mutex_unlock(ctrlr);
	nvme_request_free(ctrl->aq.rpool, req->cid);
	return err;
}

int
xnvme_be_upcie_sync_cmd_pseudo(struct xnvme_cmd_ctx *ctx, void *dbuf, size_t dbuf_nbytes,
			       void *XNVME_UNUSED(mbuf), size_t XNVME_UNUSED(mbuf_nbytes))
{
	struct xnvme_be_upcie_state *state = (void *)ctx->dev->be.state;
	struct nvme_controller *ctrl = state->ctrlr->ctrl;
	void *bar0 = ctrl->func.bars[0].region;

	switch (ctx->cmd.common.opcode) {
	case XNVME_SPEC_PSEUDO_OPC_SHOW_REGS:
		if (dbuf_nbytes != sizeof(struct xnvme_spec_ctrlr_bar)) {
			XNVME_DEBUG(
				"FAILED: dbuf_nbytes(%zu) != sizeof(struct xnvme_spec_ctrlr_bar)",
				dbuf_nbytes);
			return -EINVAL;
		}
		memcpy(dbuf, bar0, dbuf_nbytes);
		return 0;

	case XNVME_SPEC_PSEUDO_OPC_CONTROLLER_RESET:
		XNVME_DEBUG(
			"FAILED: controller-reset not supported (requires admin queue re-init)");
		return -ENOSYS;

	case XNVME_SPEC_PSEUDO_OPC_NAMESPACE_RESCAN:
		return 0;

	case XNVME_SPEC_PSEUDO_OPC_SUBSYSTEM_RESET:
		XNVME_DEBUG(
			"FAILED: subsystem-reset not supported (requires admin queue re-init)");
		return -ENOSYS;

	default:
		XNVME_DEBUG("FAILED: unsupported opcode: %d", ctx->cmd.common.opcode);
		return -ENOSYS;
	}
}
#endif

struct xnvme_be_admin g_xnvme_be_upcie_admin = {
	.id = "upcie",
#ifdef XNVME_BE_UPCIE_ENABLED
	.cmd_admin = xnvme_be_upcie_sync_cmd_admin,
	.cmd_pseudo = xnvme_be_upcie_sync_cmd_pseudo,
#else
	.cmd_admin = xnvme_be_nosys_sync_cmd_admin,
	.cmd_pseudo = xnvme_be_nosys_sync_cmd_pseudo,
#endif
};
