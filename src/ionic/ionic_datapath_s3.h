/*
 * ionic_datapath_s3.h — attach the S3 target to the data path
 *
 * ionic_datapath_s3.c implements this.  ionic_device.c calls it when
 * --backend names s3; the rest of the device doesn't need it.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IONIC_DATAPATH_S3_H
#define IONIC_DATAPATH_S3_H

#include <stdbool.h>
#include <stddef.h>

struct ionic_datapath;
struct s3_target_cfg;

/*
 * Attach the in-process S3 object store.  Once attached the emulated NIC
 * answers ARP, ping and HTTP at the configured address, and an object
 * request that carries an x-amz-rdma-token moves its payload straight
 * between the store and the guest buffer the token describes.
 *
 * Requires the Ethernet emulator, because the control plane lives on the
 * emulated wire rather than on a host socket.  Returns false and fills
 * @err on failure; the data path owns the store from then on.
 */
bool ionic_datapath_attach_s3(struct ionic_datapath *dp,
                              const struct s3_target_cfg *cfg, char *err,
                              size_t errlen);

#endif /* IONIC_DATAPATH_S3_H */
