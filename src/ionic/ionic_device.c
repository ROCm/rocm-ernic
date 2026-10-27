/*
 * ionic_device.c — the emulated ionic device, as one object
 *
 * See ionic_device.h.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ionic/ionic_device.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vfio-user/libvfio-user.h>

#include "ernic_stats.h"
#include "ernic_time.h"
#include "ionic/ionic_adminq.h"
#include "ionic/ionic_bar.h"
#include "ionic/ionic_datapath.h"
#include "ionic/ionic_datapath_nvmeof.h"
#include "ionic/ionic_datapath_s3.h"
#include "ionic/ionic_eth_emu.h"
#include "ionic/ionic_rdma_devcmd.h"
#include "nvmeof/nvmeof_target.h"
#include "rocm_ernic_compat.h"
#include "s3/s3_target.h"
#ifdef ERNIC_HAVE_P4
#include <inttypes.h>

#include "ernic_log.h"
#include "ionic/ionic_datapath_p4.h"
#include "ionic/ionic_eth_p4.h"
#include "p4/p4_devcmd.h"
#include "p4/p4_engine.h"
#endif

struct ionic_device {
    vfu_ctx_t *vfu_ctx;
    /* Borrowed; the stats file has the emulator's section while it's set */
    struct ernic_stats *stats;

    struct ionic_eth_emu *emu;
    /* Owns the admin queue context */
    struct ionic_rdma_devcmd_state *rdma;
    struct ionic_datapath *dp;
    /* ionic_device_cfg.now_ms, or ernic_monotonic_ms() */
    uint64_t (*now_ms)(void);

#ifdef ERNIC_HAVE_P4
    /* Borrowed from ionic_device_attach_p4()'s caller; set once the ports
     * are attached */
    struct p4_engine *p4_engine;
    struct ionic_eth_p4 *eth_p4;
    /* NULL unless the guest was granted a scope */
    struct p4_devcmd *p4_devcmd;
#endif
};

/* ---- Targets ----------------------------------------------------------- */

/* The options after the ':' in a --backend string, or NULL if it has none. */
static const char *backend_opts(const char *backend)
{
    const char *colon = backend ? strchr(backend, ':') : NULL;

    return colon ? colon + 1 : NULL;
}

/*
 * Parse the NVMe-oF target's options, then start it on @dp, or, with a
 * NULL @dp, only check that it could be.
 */
static bool nvmeof_setup(struct ionic_datapath *dp, const char *opts, char *err,
                         size_t errlen)
{
    struct nvmeof_target_cfg cfg;
    char why[256] = "";
    bool ok;

    nvmeof_target_cfg_defaults(&cfg);
    ok = nvmeof_target_cfg_parse(&cfg, opts, why, sizeof(why)) &&
         (dp ? ionic_datapath_attach_nvmeof(dp, &cfg, why, sizeof(why))
             : ionic_datapath_nvmeof_cfg_check(&cfg, why, sizeof(why)));
    free(cfg.file); /* the target keeps its own copy */
    if (!ok)
        snprintf(err, errlen, "nvmeof backend: %s", why);
    return ok;
}

/* nvmeof_setup() for the S3 target */
static bool s3_setup(struct ionic_datapath *dp, const char *opts, char *err,
                     size_t errlen)
{
    struct s3_target_cfg cfg;
    char why[256] = "";
    bool ok;

    s3_target_cfg_defaults(&cfg);
    ok = s3_target_cfg_parse(&cfg, opts, why, sizeof(why)) &&
         (!dp || ionic_datapath_attach_s3(dp, &cfg, why, sizeof(why)));
    if (!ok)
        snprintf(err, errlen, "s3 backend: %s", why);
    return ok;
}

/* Set up, or with a NULL @dp check, the target @backend names, if any. */
static bool target_setup(struct ionic_datapath *dp, const char *backend,
                         char *err, size_t errlen)
{
    const char *name = pvrdma_backend_name(backend);

    if (name && !strcmp(name, "nvmeof"))
        return nvmeof_setup(dp, backend_opts(backend), err, errlen);
    if (name && !strcmp(name, "s3"))
        return s3_setup(dp, backend_opts(backend), err, errlen);
    return true;
}

bool ionic_device_target_check(const char *backend, char *err, size_t errlen)
{
    return target_setup(NULL, backend, err, errlen);
}

/* ---- Construction / destruction ---------------------------------------- */

struct ionic_device *ionic_device_create(const struct ionic_device_cfg *cfg,
                                         char *err, size_t errlen)
{
    struct ionic_device *dev = calloc(1, sizeof(*dev));

    if (!dev) {
        snprintf(err, errlen, "out of memory for the ionic device");
        return NULL;
    }
    dev->vfu_ctx = cfg->vfu_ctx;
    dev->now_ms = cfg->now_ms ? cfg->now_ms : ernic_monotonic_ms;

    dev->emu = ionic_eth_emu_create(cfg->vfu_ctx, IONIC_BAR2_DB_SIZE);
    dev->rdma = ionic_rdma_devcmd_create(cfg->vfu_ctx);
    if (dev->emu)
        dev->dp = ionic_datapath_create(cfg->vfu_ctx, dev->emu);
    if (!dev->emu || !dev->rdma || !dev->dp) {
        snprintf(err, errlen, "out of memory for the ionic device");
        ionic_device_destroy(dev);
        return NULL;
    }

    /* The front of the device: registers, device commands and doorbells.
     * RDMA devcmds go to the handler, RDMA doorbells to the data path. */
    ionic_eth_emu_set_stats(dev->emu, cfg->stats);
    if (cfg->mac)
        ionic_eth_emu_set_mac(dev->emu, cfg->mac);
    if (cfg->profile)
        ionic_eth_emu_set_profile(dev->emu, cfg->profile);
    ionic_eth_emu_set_faults(dev->emu, cfg->faults);
    ionic_eth_emu_register_rdma_handler(dev->emu, ionic_rdma_devcmd_dispatch,
                                        dev->rdma);
    ionic_eth_emu_register_datapath(dev->emu, dev->dp);

    /* The handler raises EQ interrupts through the emulator, and hands each
     * admin queue context it creates the rest: the guest's CREATE_CQ, QP
     * and MR describe rings the data path then serves. */
    ionic_rdma_devcmd_set_eth_emu(dev->rdma, dev->emu);
    ionic_rdma_devcmd_set_pvrdma(dev->rdma, cfg->pvrdma);
    ionic_rdma_devcmd_set_stats(dev->rdma, cfg->stats);
    ionic_rdma_devcmd_set_faults(dev->rdma, cfg->faults);
    ionic_rdma_devcmd_set_datapath(dev->rdma, dev->dp);

    ionic_datapath_set_pvrdma(dev->dp, cfg->pvrdma);
    ionic_datapath_set_stats(dev->dp, cfg->stats);

    /* The Ethernet side's LIF and port counters go in the stats file too */
    dev->stats = cfg->stats;
    ernic_stats_set_section(dev->stats, ionic_eth_emu_write_stats, dev->emu);

    if (!target_setup(dev->dp, cfg->backend, err, errlen)) {
        ionic_device_destroy(dev);
        return NULL;
    }
    return dev;
}

#ifdef ERNIC_HAVE_P4
/*
 * One machine-parseable line, so a black-box test can tell a frame the
 * pipeline forwarded from one it dropped.  The counters live inside the
 * process and have no other way out.
 */
static void report_p4_stats(const struct ionic_device *dev)
{
    struct p4_stats es;
    struct ionic_eth_p4_stats ps;

    p4_engine_get_stats(dev->p4_engine, &es);
    ionic_eth_p4_get_stats(dev->eth_p4, &ps);
    ernic_startup_report(
        "rocm-ernic: P4 stats: host_in=%" PRIu64 " host_out=%" PRIu64
        " uplink_in=%" PRIu64 " uplink_out=%" PRIu64 " dropped=%" PRIu64
        " too_big=%" PRIu64 " parse_errors=%" PRIu64 " to_guest=%" PRIu64
        " to_net=%" PRIu64 " to_upstream=%" PRIu64 " egress_errors=%" PRIu64,
        es.frames_in[P4_PORT_HOST], es.frames_out[P4_PORT_HOST],
        es.frames_in[P4_PORT_UPLINK], es.frames_out[P4_PORT_UPLINK],
        es.frames_dropped, es.frames_too_big, es.parse_errors, ps.to_guest,
        ps.to_net, ps.to_upstream, ps.egress_errors);
}
#endif

void ionic_device_destroy(struct ionic_device *dev)
{
    if (!dev)
        return;

    ernic_stats_set_section(dev->stats, NULL, NULL);

#ifdef ERNIC_HAVE_P4
    if (dev->p4_engine) {
        report_p4_stats(dev);
        /* Before the data path, which clears the Tx filter slot the P4
         * attachment is holding on the S3 target's behalf. */
        ionic_eth_p4_detach(dev->eth_p4);
        ionic_eth_emu_register_p4_handler(dev->emu, NULL, NULL);
        p4_devcmd_destroy(dev->p4_devcmd);
    }
#endif

    /* The data path first: it unregisters its mesh receive callback from
     * the RDMA core, and both it and the handler hold pointers into the
     * emulator. */
    ionic_datapath_destroy(dev->dp);
    ionic_rdma_devcmd_destroy(dev->rdma);
    ionic_eth_emu_destroy(dev->emu);
    free(dev);
}

#ifdef ERNIC_HAVE_P4
bool ionic_device_attach_p4(struct ionic_device *dev, struct p4_engine *eng,
                            enum p4_scope guest_scope, char *err, size_t errlen)
{
    struct ionic_eth_p4_upstream upstream;

    if (!dev || !eng || dev->p4_engine) {
        snprintf(err, errlen, "P4 engine missing or already attached");
        return false;
    }

    memset(&upstream, 0, sizeof(upstream));
    ionic_eth_emu_get_tx_filter(dev->emu, &upstream.fn, &upstream.ctx);
    dev->eth_p4 =
        ionic_eth_p4_attach(dev->emu, eng, upstream.fn ? &upstream : NULL);
    if (!dev->eth_p4) {
        snprintf(err, errlen, "failed to attach the P4 engine");
        return false;
    }
    dev->p4_engine = eng;

    /* The RDMA path shares the engine, so one program governs both
     * planes. */
    ionic_datapath_set_p4_engine(dev->dp, eng);

    if (guest_scope != P4_SCOPE_NONE) {
        dev->p4_devcmd = p4_devcmd_create(dev->vfu_ctx, eng, guest_scope);
        if (!dev->p4_devcmd) {
            snprintf(err, errlen, "failed to create the P4 command surface");
            return false;
        }
        ionic_eth_emu_register_p4_handler(dev->emu, p4_devcmd_dispatch,
                                          dev->p4_devcmd);
    }
    return true;
}
#endif

/* ---- The main loop ----------------------------------------------------- */

void ionic_device_poll(struct ionic_device *dev)
{
    if (!dev)
        return;

    ionic_eth_emu_poll_rx(dev->emu);
    ionic_eth_emu_publish_stats(dev->emu, dev->now_ms());
    ionic_adminq_poll(ionic_rdma_devcmd_get_adminq_ctx(dev->rdma),
                      dev->vfu_ctx);
    ionic_datapath_poll(dev->dp);
}

bool ionic_device_has_work(struct ionic_device *dev)
{
    return dev && ionic_datapath_has_work(dev->dp);
}

/* ---- The parts --------------------------------------------------------- */

struct ionic_eth_emu *ionic_device_eth_emu(const struct ionic_device *dev)
{
    return dev ? dev->emu : NULL;
}

struct ionic_datapath *ionic_device_datapath(const struct ionic_device *dev)
{
    return dev ? dev->dp : NULL;
}

struct ionic_rdma_devcmd_state *ionic_device_rdma_devcmd(
    const struct ionic_device *dev)
{
    return dev ? dev->rdma : NULL;
}
