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
int
xnvme_be_upcie_attach_cmd_admin(struct xnvme_cmd_ctx *ctx, void *dbuf, size_t dbuf_nbytes)
{
	struct xnvme_be_upcie_state *state = (void *)ctx->dev->be.state;
	struct upcie_attach_desc *adesc = state->ctrlr->adesc;
	uint8_t opc = ctx->cmd.common.opcode;
	uint8_t cns;

	memset(&ctx->cpl, 0, sizeof(ctx->cpl));

	if (opc != XNVME_SPEC_ADM_OPC_IDFY) {
		if (dbuf && dbuf_nbytes) {
			memset(dbuf, 0, dbuf_nbytes);
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
		memcpy(dbuf, adesc->idfy_ns, UPCIE_ATTACH_IDFY_NBYTES);
		return 0;
	case XNVME_SPEC_IDFY_CTRLR:
		memcpy(dbuf, adesc->idfy_ctrlr, UPCIE_ATTACH_IDFY_NBYTES);
		return 0;
	case 0x02: ///< Active Namespace ID list
		memset(dbuf, 0, dbuf_nbytes);
		((uint32_t *)dbuf)[0] = adesc->nsid;
		return 0;
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
	struct nvme_controller *ctrl;
	struct nvme_command *cmd = (struct nvme_command *)&ctx->cmd;
	struct nvme_completion *cpl = (struct nvme_completion *)&ctx->cpl;
	int err;

	if (state->ctrlr->attach) {
		return xnvme_be_upcie_attach_cmd_admin(ctx, dbuf, dbuf_nbytes);
	}

	ctrl = state->ctrlr->ctrl;
	if (dbuf) {
		err = nvme_qpair_submit_sync_contig_prps(&ctrl->aq, &g_upcie_rte.heap, dbuf,
							 dbuf_nbytes, cmd, ctrl->timeout_ms, cpl);
	} else {
		err = nvme_qpair_submit_sync(&ctrl->aq, cmd, ctrl->timeout_ms, cpl);
	}

	if (err || xnvme_cmd_ctx_cpl_status(ctx)) {
		XNVME_DEBUG("FAILED: nvme_submit_sync(); err(%d)", err);
		return err;
	}

	return 0;
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
