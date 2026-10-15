/*
 * Private test fixture for the TCP ionic hooks.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef ERNIC_TCP_IONIC_TEST_H
#define ERNIC_TCP_IONIC_TEST_H

#include <stdbool.h>
#include "rdma_backend.h"
#include "rdma_rm.h"
#include "hw/pci/pci.h"
#include "qemu/thread.h"

typedef struct TcpBackendPrivate TcpBackendPrivate;
TcpBackendPrivate *tcp_ionic_test_new(void);
bool tcp_ionic_test_matches(TcpBackendPrivate *priv, tcp_ionic_recv_fn fn,
                            void *opaque);
void tcp_ionic_test_free(TcpBackendPrivate *priv);

#endif
