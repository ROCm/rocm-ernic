/*
 * ionic_datapath_p4.h — put the P4 pipeline on the data path
 *
 * ionic_datapath_p4.c implements these.  Built only with ERNIC_BUILD_P4.
 * ionic_device.c gives the data path its engine; the pipeline's fp8 extern
 * calls the arithmetic and the conversion, and the tests call them
 * directly.  The rest of the device doesn't need them.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef IONIC_DATAPATH_P4_H
#define IONIC_DATAPATH_P4_H

#include <stdint.h>

struct ionic_datapath;
struct p4_engine;

/*
 * Give the data path the pipeline that governs RDMA operations, or NULL to
 * take it away.
 *
 * Borrowed, not owned: the engine outlives nothing here, and the caller
 * must clear it before destroying the engine.  Until one is set, and for a
 * program that declares no rdma_metadata, the data path behaves exactly as
 * it does without P4 built at all.
 *
 * What the pipeline may do to an operation is decide: it runs after the
 * operation's own memory has been checked, so a program never sees a
 * request that is already a protection error, and it cannot name memory --
 * an extern it calls is given the regions the work request itself named.
 */
void ionic_datapath_set_p4_engine(struct ionic_datapath *dp,
                                  struct p4_engine *eng);

/*
 * The most a single fp8 operation or conversion may move, in elements.  The
 * work happens on the thread that also services the vfio-user poll loop, so
 * this is a bound on how long a guest may stall that loop, not a bound on
 * memory -- the streaming would take any length.  A megabyte of elements is
 * a visible pause; this is the point at which that was judged acceptable,
 * and it is one constant to revisit when it is not.
 */
#define IONIC_FP8_MAP_MAX 262144u

/*
 * Apply an fp8 operation elementwise across two registered memory regions
 * and write the result to a third.
 *
 * This is in-network compute: the NIC reads both operand regions, combines
 * them pointwise, and writes a result region of the same length, instead
 * of the host reading two buffers and writing a third.  Combining two
 * partial buffers is what a collective does, so this is the primitive an
 * allreduce is built from.
 *
 * The regions are streamed in blocks rather than staged whole, so the
 * length is bounded by what a guest may ask the poll loop to do in one go
 * rather than by how much will fit on the stack.
 *
 * This is the implementation of the fp8 extern's map(): a program's action
 * chooses @op, and the work request, not the program, names the regions
 * (dp_p4_fp8_map() in ionic_datapath_p4.c).
 *
 * @op is an enum p4_fp8_op.  @len is the length of each region in bytes,
 * one fp8 value per byte, and the destination region is the same length.
 *
 * Returns 0, or a negative errno: -EINVAL for an argument out of range,
 * -EACCES for a key the guest may not name here, -EFAULT when a region
 * could not be read or written in full.
 */
int ionic_datapath_fp8_map(struct ionic_datapath *dp, uint8_t op,
                           uint32_t a_lkey, uint64_t a_va, uint32_t b_lkey,
                           uint64_t b_va, uint32_t len, uint32_t c_rkey,
                           uint64_t c_va);

/*
 * Convert a region of floating point from one format to another,
 * elementwise, in either direction.
 *
 * @src_fmt and @dst_fmt are enum p4_fmt values.  @in_len bytes at
 * (@a_lkey, @a_va) hold however many elements of @src_fmt that is; the
 * result is the same number of @dst_fmt elements at (@c_rkey, @c_va),
 * which is longer, shorter or the same length depending on the pair.
 *
 * This is the only operation whose destination length is not its source
 * length, so each region is checked against its own, in full, before a
 * byte moves.  Returns 0, or a negative errno.
 *
 * The destination may not overlap the source, and nothing here checks
 * that it does not.  The conversion streams a chunk at a time -- read,
 * convert, write -- so a destination that overlaps the part of the
 * source still to be read is overwritten before it is used, and the
 * operation then completes successfully with wrong results from that
 * point on.  Widening makes it easy to reach: fp16 to fp32 into the same
 * address writes two bytes for every one it has read, so the second
 * chunk's input is gone before the first chunk is finished.
 *
 * Not checked because checking it properly is not a range comparison.
 * Two different keys may be registered over the same guest pages, so
 * overlap has to be decided on what the keys resolve to rather than on
 * the keys themselves, and the honest fix is to stage the whole source
 * before writing any of it.  Until then this is the guest corrupting its
 * own memory with its own keys -- it can reach nothing it has not
 * registered, and every region is still checked for that -- so it is
 * recorded here rather than guessed at.
 */
int ionic_datapath_fmt_convert(struct ionic_datapath *dp, uint8_t src_fmt,
                               uint8_t dst_fmt, uint32_t a_lkey, uint64_t a_va,
                               uint32_t in_len, uint32_t c_rkey, uint64_t c_va);

#endif /* IONIC_DATAPATH_P4_H */
