/*
 * ionic_datapath_s3.c — the data path's side of the S3 target
 *
 * With --backend s3 the guest reaches an object store in this process.  Its
 * HTTP control plane runs over the minimal TCP/IP stack in s3/s3_tcp.c,
 * which claims the guest's transmitted frames for the store's address and
 * answers through the guest's receive queue; object payloads move by RDMA
 * through the DMA callbacks, against the rkey each request names.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <syslog.h>

#include <vfio-user/libvfio-user.h>

#include "ionic_datapath.h"
#include "ionic_datapath_priv.h"
#include "ionic_eth_emu.h"
#include "s3/s3_tcp.h"
#include "s3/s3_target.h"

static const struct s3_dma_ops dp_s3_dma_ops = {
    .from_host = ionic_dp_target_from_host,
    .to_host = ionic_dp_target_to_host,
};

/*
 * The control plane's replies go back to the guest as received frames.  One
 * the emulator can't queue is lost as on a wire: s3/s3_tcp.c retransmits
 * what the guest doesn't acknowledge, and a full queue is counted in the
 * port's statistics.
 */
static void dp_s3_tx_frame(void *ctx, const void *frame, size_t len)
{
    struct ionic_datapath *dp = ctx;

    (void)ionic_eth_emu_queue_rx_frame(dp->eth_emu, frame, len);
}

static bool dp_s3_tx_filter(void *ctx, const void *frame, size_t len)
{
    struct ionic_datapath *dp = ctx;

    return s3_tcp_rx_frame(dp->s3_tcp, frame, len, ionic_dp_now_ms(dp));
}

bool ionic_datapath_attach_s3(struct ionic_datapath *dp,
                              const struct s3_target_cfg *cfg, char *err,
                              size_t errlen)
{
    if (!dp || !cfg)
        return false;

    if (!dp->eth_emu) {
        snprintf(err, errlen,
                 "the s3 backend needs the Ethernet emulator for its "
                 "control plane");
        return false;
    }

    dp->s3 = s3_target_create(cfg, err, errlen);
    if (!dp->s3)
        return false;

    struct s3_tcp_cfg ncfg;
    s3_tcp_cfg_from_target(&ncfg, dp->s3);
    dp->s3_tcp = s3_tcp_create(&ncfg, dp->s3, &dp_s3_dma_ops, dp,
                               dp_s3_tx_frame, dp, err, errlen);
    if (!dp->s3_tcp) {
        s3_target_destroy(dp->s3);
        dp->s3 = NULL;
        return false;
    }

    ionic_eth_emu_register_tx_filter(dp->eth_emu, dp_s3_tx_filter, dp);

    vfu_log(dp->vfu_ctx, LOG_INFO,
            "ionic_datapath: s3 bucket '%s' %" PRIu64
            " bytes at http://%u.%u.%u.%u:%u/ mac "
            "%02x:%02x:%02x:%02x:%02x:%02x",
            cfg->bucket, cfg->capacity, cfg->traddr >> 24,
            (cfg->traddr >> 16) & 0xff, (cfg->traddr >> 8) & 0xff,
            cfg->traddr & 0xff, cfg->trsvcid, ncfg.mac[0], ncfg.mac[1],
            ncfg.mac[2], ncfg.mac[3], ncfg.mac[4], ncfg.mac[5]);
    return true;
}

void ionic_dp_s3_detach(struct ionic_datapath *dp)
{
    if (dp->eth_emu && dp->s3_tcp)
        ionic_eth_emu_register_tx_filter(dp->eth_emu, NULL, NULL);
    s3_tcp_destroy(dp->s3_tcp);
    s3_target_destroy(dp->s3);
}

bool ionic_dp_s3_has_work(struct ionic_datapath *dp)
{
    return dp->s3_tcp && s3_tcp_has_work(dp->s3_tcp);
}

void ionic_dp_s3_poll(struct ionic_datapath *dp)
{
    if (dp->s3_tcp)
        s3_tcp_poll(dp->s3_tcp, ionic_dp_now_ms(dp));
}
