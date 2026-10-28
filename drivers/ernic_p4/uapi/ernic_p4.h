/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* Copyright (C) Advanced Micro Devices, Inc. */
/*
 * ernic_p4.h - userspace interface to the rocm-ernic P4 pipeline
 *
 * /dev/ernic-p4 is a thin pipe onto the emulated NIC's P4 control
 * surface.  Each ioctl becomes one ionic device command: identify,
 * query, or an RPC on an endpoint.  The driver stages buffers for DMA
 * and copies them; it does not interpret their contents, so the payload
 * formats are the device's and are documented in docs/p4.rst rather
 * than here.
 *
 * Buffers are named by userspace pointer and length.  The driver bounces
 * them through its own coherent memory rather than pinning the caller's,
 * because these are small and infrequent -- a program arrives in chunks
 * of at most a few kilobytes -- and bouncing keeps a page the caller
 * might free out of the device's reach.
 */

#ifndef _UAPI_ERNIC_P4_H
#define _UAPI_ERNIC_P4_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define ERNIC_P4_IOC_MAGIC 0xE4

/*
 * The most one call may carry in each direction.  These mirror the
 * device's own limits, which ERNIC_P4_IOC_IDENT reports; the driver
 * enforces its own ceiling as well, so a device claiming something
 * larger cannot talk it into an unbounded allocation.
 */
#define ERNIC_P4_REQ_MAX (64u * 1024u)
#define ERNIC_P4_RESP_MAX (4u * 1024u)

/**
 * struct ernic_p4_ident - what the device is and what this guest may do
 * @buf:    userspace address the device's identify structure is copied to
 * @len:    bytes of @buf; the device truncates to what it has
 * @status: the device's status byte, valid when the ioctl returns 0
 * @rsvd:   must be zero
 */
struct ernic_p4_ident {
	__u64 buf;
	__u32 len;
	__u32 status;
};

/**
 * struct ernic_p4_query - enumerate endpoints, or one endpoint's operations
 * @buf:    userspace address the answer is copied to
 * @len:    bytes of @buf
 * @entity: 0 for the endpoint list, 1 for the operations of @ep
 * @ep:     the endpoint, when @entity is 1
 * @status: the device's status byte
 * @rsvd:   must be zero
 */
struct ernic_p4_query {
	__u64 buf;
	__u32 len;
	__u32 entity;
	__u32 ep;
	__u32 status;
};

/**
 * struct ernic_p4_rpc - one call on an endpoint
 * @req:      userspace address of the request, or 0
 * @req_len:  bytes of @req
 * @resp:     userspace address the response is copied to, or 0
 * @resp_len: bytes of @resp on entry; bytes written on return
 * @ep:       endpoint id
 * @op:       operation id
 * @scope:    the scope being asked for; the device refuses a request
 *            whose scope is below what the operation needs or above
 *            what the operator granted, and refuses any operation the
 *            operator's grant does not cover
 * @status:   the device's status byte
 */
struct ernic_p4_rpc {
	__u64 req;
	__u32 req_len;
	__u32 rsvd;
	__u64 resp;
	__u32 resp_len;
	__u32 ep;
	__u32 op;
	__u32 scope;
	__u32 status;
	__u32 rsvd2;
};

#define ERNIC_P4_IOC_IDENT \
	_IOWR(ERNIC_P4_IOC_MAGIC, 0x01, struct ernic_p4_ident)
#define ERNIC_P4_IOC_QUERY \
	_IOWR(ERNIC_P4_IOC_MAGIC, 0x02, struct ernic_p4_query)
#define ERNIC_P4_IOC_RPC _IOWR(ERNIC_P4_IOC_MAGIC, 0x03, struct ernic_p4_rpc)

#endif /* _UAPI_ERNIC_P4_H */
