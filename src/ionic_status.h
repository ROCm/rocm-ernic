/*
 * ionic completion status codes.
 *
 * enum ionic_status_code from the kernel's ionic_if.h (pinned kernel ref).
 * A devcmd or admin CQE carries one of these in its status byte; the driver
 * logs it by name (ionic_error_to_str) and maps it to an errno.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IONIC_STATUS_H
#define IONIC_STATUS_H

enum ionic_status_code {
    IONIC_RC_SUCCESS = 0,   /* Success */
    IONIC_RC_EVERSION = 1,  /* Incorrect version for request */
    IONIC_RC_EOPCODE = 2,   /* Invalid cmd opcode */
    IONIC_RC_EIO = 3,       /* I/O error */
    IONIC_RC_EPERM = 4,     /* Permission denied */
    IONIC_RC_EQID = 5,      /* Bad qid */
    IONIC_RC_EQTYPE = 6,    /* Bad qtype */
    IONIC_RC_ENOENT = 7,    /* No such element */
    IONIC_RC_EINTR = 8,     /* operation interrupted */
    IONIC_RC_EAGAIN = 9,    /* Try again */
    IONIC_RC_ENOMEM = 10,   /* Out of memory */
    IONIC_RC_EFAULT = 11,   /* Bad address */
    IONIC_RC_EBUSY = 12,    /* Device or resource busy */
    IONIC_RC_EEXIST = 13,   /* object already exists */
    IONIC_RC_EINVAL = 14,   /* Invalid argument */
    IONIC_RC_ENOSPC = 15,   /* No space left or alloc failure */
    IONIC_RC_ERANGE = 16,   /* Parameter out of range */
    IONIC_RC_BAD_ADDR = 17, /* Descriptor contains a bad ptr */
    IONIC_RC_DEV_CMD = 18,  /* Device cmd attempted on AdminQ */
    IONIC_RC_ENOSUPP = 19,  /* Operation not supported */
    IONIC_RC_ERROR = 29,    /* Generic error */
    IONIC_RC_ERDMA = 30,    /* Generic RDMA error */
    IONIC_RC_EVFID = 31,    /* VF ID does not exist */
    IONIC_RC_EBAD_FW = 32,  /* FW file is invalid or corrupted */
};

#endif /* IONIC_STATUS_H */
