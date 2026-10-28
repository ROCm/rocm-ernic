/*
 * ionic_datapath_nvmeof.c — the data path's side of the NVMe-oF target
 *
 * With --backend nvmeof the guest's nvme-rdma initiator talks to a
 * controller in this process.  Its CM MADs arrive on the GSI QP and its
 * command capsules on the RC QP the handshake bound; ionic_datapath.c hands
 * both here, and the replies go back into the guest's receive queues as
 * from a real peer.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include <vfio-user/libvfio-user.h>

#include "byteorder.h"
#include "ernic_limits.h"
#include "ionic/ionic_datapath_fw.h"
#include "ionic/ionic_datapath_nvmeof.h"
#include "ionic/ionic_datapath_priv.h"
#include "net/net_headers.h"
#include "nvmeof/nvmeof.h"
#include "nvmeof/nvmeof_cm.h"
#include "nvmeof/nvmeof_target.h"
#include "rocm_ernic_compat.h"

bool ionic_datapath_nvmeof_cfg_check(const struct nvmeof_target_cfg *cfg,
                                     char *err, size_t errlen)
{
    if (cfg->max_queues <= IONIC_MAX_MR_QUEUES)
        return true;

    /* The ceiling is the memory-region budget, not the NVMe spec: the
     * initiator pre-allocates IONIC_MR_PER_QUEUE regions per queue at
     * connect time.  Refusing here turns what was a bare "invalid
     * arguments/configuration" from nvme connect, long after startup, into a
     * message that names the real limit. */
    snprintf(err, errlen,
             "queues must be 1..%d, one per %d memory regions in a "
             "table of %u (got '%" PRIu16 "')",
             IONIC_MAX_MR_QUEUES, IONIC_MR_PER_QUEUE, ERNIC_MAX_MR,
             cfg->max_queues);
    return false;
}

bool ionic_datapath_attach_nvmeof(struct ionic_datapath *dp,
                                  const struct nvmeof_target_cfg *cfg,
                                  char *err, size_t errlen)
{
    if (!dp || !cfg)
        return false;

    if (!ionic_datapath_nvmeof_cfg_check(cfg, err, errlen))
        return false;

    dp->nvmeof = nvmeof_target_create(cfg, err, errlen);
    if (!dp->nvmeof)
        return false;

    dp->nvmeof_cm = nvmeof_cm_create(dp->nvmeof, cfg->traddr, cfg->trsvcid);
    if (!dp->nvmeof_cm) {
        nvmeof_target_destroy(dp->nvmeof);
        dp->nvmeof = NULL;
        snprintf(err, errlen, "out of memory");
        return false;
    }

    /* The target's copy has the size an existing image file settled on. */
    cfg = nvmeof_target_config(dp->nvmeof);
    vfu_log(dp->vfu_ctx, LOG_INFO,
            "ionic_datapath: nvmeof target '%s' %" PRIu64
            " bytes bs=%u at %u.%u.%u.%u:%u",
            cfg->subnqn, cfg->size, cfg->block_size, cfg->traddr >> 24,
            (cfg->traddr >> 16) & 0xff, (cfg->traddr >> 8) & 0xff,
            cfg->traddr & 0xff, cfg->trsvcid);
    return true;
}

void ionic_dp_nvmeof_detach(struct ionic_datapath *dp)
{
    nvmeof_cm_destroy(dp->nvmeof_cm);
    nvmeof_target_destroy(dp->nvmeof);
}

void ionic_dp_nvmeof_drop_qp(struct ionic_datapath *dp, uint32_t qp_id)
{
    if (dp->nvmeof_cm)
        nvmeof_cm_drop_qp(dp->nvmeof_cm, qp_id);
}

static const struct nvmeof_dma_ops dp_nvmeof_dma_ops = {
    .from_host = ionic_dp_target_from_host,
    .to_host = ionic_dp_target_to_host,
};

/* Build the global route header a UD receive is expected to carry. */
static void dp_nvmeof_grh(const struct ionic_datapath *dp,
                          uint8_t grh[IB_GRH_SIZE], uint32_t paylen)
{
    /* sgid is the controller, dgid the guest. */
    ionic_dp_grh_build(grh, paylen, dp->nvmeof_sgid, dp->nvmeof_dgid);
}

/*
 * Remember which GIDs the guest is using.  A CM REQ names both ends of the
 * path it wants, and it is the only MAD that does, so the GRH on every reply
 * after it is built from what the REQ said.
 */
static void dp_nvmeof_learn_gids(struct ionic_datapath *dp, const uint8_t *mad)
{
    if (ernic_get_be16(mad + 16) != 0x0010) /* CM_ATTR_REQ */
        return;
    memcpy(dp->nvmeof_dgid, mad + 80,
           ERNIC_GID_LEN); /* primary local  = guest */
    memcpy(dp->nvmeof_sgid, mad + 96, ERNIC_GID_LEN); /* primary remote = us */
}

/*
 * A CM MAD the guest put on its GSI QP.  Returns true once the responder has
 * claimed it, whether or not a reply went back, so the caller does not also
 * try to deliver it to a peer QP that does not exist.
 */
bool ionic_dp_nvmeof_gsi(struct ionic_datapath *dp, struct ionic_qp_ring *q,
                         uint32_t qp_id, const struct dp_sge_list *src)
{
    if (!dp->nvmeof_cm || src->total < IB_MAD_SIZE)
        return false;

    uint8_t mad[IB_MAD_SIZE];
    if (ionic_dp_gather(dp, src, mad, IB_MAD_SIZE) != IB_MAD_SIZE)
        return false;

    uint8_t rsp[IB_MAD_SIZE];
    struct nvmeof_cm_result res;
    if (!nvmeof_cm_handle_mad(dp->nvmeof_cm, mad, IB_MAD_SIZE, rsp, &res))
        return false;

    dp_nvmeof_learn_gids(dp, mad);

    switch (res.action) {
    case NVMEOF_CM_ACT_BIND:
        vfu_log(dp->vfu_ctx, LOG_INFO,
                "ionic_datapath: nvmeof queue %u bound to QP %u (ctrl QP %u)",
                res.nvme_qid, res.guest_qpn, res.local_qpn);
        break;
    case NVMEOF_CM_ACT_UNBIND:
        vfu_log(dp->vfu_ctx, LOG_INFO, "ionic_datapath: nvmeof released QP %u",
                res.guest_qpn);
        break;
    case NVMEOF_CM_ACT_NONE:
    case NVMEOF_CM_ACT_ESTABLISHED:
    default:
        break;
    }

    if (!res.rsp_len)
        return true;

    uint8_t buf[IB_GRH_SIZE + IB_MAD_SIZE];
    dp_nvmeof_grh(dp, buf, (uint32_t)res.rsp_len);
    memcpy(buf + IB_GRH_SIZE, rsp, res.rsp_len);

    struct dp_sge_list in = {.count = 0,
                             .total = (uint32_t)(IB_GRH_SIZE + res.rsp_len)};
    if (ionic_dp_deliver_recv(dp, q, qp_id,
                              1 /* the peer's GSI QP is also QP1 */, &in, buf,
                              CQE_RECV_OP_SEND, 0) < 0)
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: nvmeof CM reply dropped, no GSI receive");
    return true;
}

/*
 * An NVMe command capsule on a QP the CM handshake bound to a controller
 * queue.  The controller executes it synchronously -- pulling or pushing data
 * through the command's own keyed SGL -- and the response capsule goes back
 * as a SEND into the initiator's next posted receive.
 */
bool ionic_dp_nvmeof_rc_send(struct ionic_datapath *dp, struct ionic_qp_ring *q,
                             uint32_t qp_id, const struct dp_sge_list *src)
{
    if (!dp->nvmeof || !q->dest_valid ||
        !nvmeof_cm_is_target_qpn(q->dest_qp_id))
        return false;

    struct nvmeof_queue *nq = nvmeof_target_find_queue(dp->nvmeof, qp_id);
    if (!nq) {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u has no nvmeof queue", qp_id);
        return true;
    }

    /* Only the admin Connect ever needs room past the SQE, and only if the
     * host ignores the ioccsz we advertise and sends its data in capsule. */
    uint8_t capsule[NVME_SQE_SIZE + 1024];
    uint32_t len =
        src->total < sizeof(capsule) ? src->total : (uint32_t)sizeof(capsule);
    if (ionic_dp_gather(dp, src, capsule, len) != len) {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u capsule fetch failed", qp_id);
        return true;
    }

    uint8_t rsp[NVME_CQE_SIZE];
    int r = nvmeof_queue_exec(nq, capsule, len, &dp_nvmeof_dma_ops, dp, rsp);
    if (r == NVMEOF_EXEC_HELD)
        return true; /* an AER: answered only when an event occurs */
    if (r < 0) {
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u bad capsule (%u bytes)", qp_id, len);
        return true;
    }

    struct dp_sge_list in = {.count = 0, .total = NVME_CQE_SIZE};
    if (ionic_dp_deliver_recv(dp, q, qp_id, q->dest_qp_id, &in, rsp,
                              CQE_RECV_OP_SEND, 0) < 0)
        vfu_log(dp->vfu_ctx, LOG_WARNING,
                "ionic_datapath: QP %u has no receive for a response capsule",
                qp_id);
    return true;
}
