/*
 * ernic_limits.h - how many of each RDMA resource the device holds
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef ERNIC_LIMITS_H
#define ERNIC_LIMITS_H

/*
 * The one place the device's resource limits are set.
 *
 * Each limit is both a promise and a table size, and the two have to agree.
 * The LIF identity tells the guest driver how many of each it may create
 * (ionic_eth_emu_handle_lif_identify()), and every table that holds one has
 * to hold that many: the admin queue's id maps (ionic_adminq_priv.h), the
 * data path's tables (ionic_datapath_create()), and the resource manager's,
 * which rdma_rm_init() sizes from each backend's query_device(). Whichever
 * is smallest is the real ceiling, so a table smaller than the promise fails
 * the guest at a count it was told it could reach.
 *
 * These were once set in each of those places, and disagreed: the guest was
 * told 32768 QPs and 65536 CQs, the data path held as many, the resource
 * manager 1024 of each, and the admin queue's maps 256, so the 257th CREATE
 * failed on a device that had promised 128 times that. Everything now takes
 * its size from here; raising a limit costs memory in the data path, and a
 * longer search of the admin queue's maps, which are scanned linearly.
 */

/*
 * Admin queues. The driver creates up to this many, one per CPU. An int,
 * unlike the rest, as the code that indexes them counts in ints.
 */
#define ERNIC_MAX_AQ 4

/*
 * QPs and CQs. The guest driver picks their ids itself, below the counts in
 * the LIF identity, so each table must hold every id below its limit; the
 * data path's tables are indexed by them.
 *
 * Both must be powers of two. The driver splits each id space in half, one
 * half per udma, and interleaves the halves in groups of 2^udma_shift
 * (ionic_bitid_to_qid()). It rounds the half up to a power of two, so for
 * any other count it hands out ids at or past the count.
 * ionic_eth_emu_priv.h checks that the groups fit.
 *
 * The driver spends a CQ per udma, two, on each verbs CQ, and reports
 * max_cq as half the CQ count, so twice as many CQs as QPs lets nearly
 * every QP have a CQ of its own. The admin queues' CQs come out of the same
 * ids, ERNIC_MAX_AQ of them.
 *
 * 256 QPs is what every table could already hold, the admin queue's map
 * being the smallest, so the guest lost none it could have used when the
 * promise came down to it.
 */
#define ERNIC_MAX_QP 256u
#define ERNIC_MAX_CQ 512u

_Static_assert((ERNIC_MAX_QP & (ERNIC_MAX_QP - 1u)) == 0,
               "the guest driver needs a power-of-two QP count");
_Static_assert((ERNIC_MAX_CQ & (ERNIC_MAX_CQ - 1u)) == 0,
               "the guest driver needs a power-of-two CQ count");
_Static_assert(ERNIC_MAX_CQ > ERNIC_MAX_AQ,
               "the admin queues' CQs would leave none for the guest's");

/*
 * SRQs. The device assigns their ids (see IONIC_SRQ_QID_BASE in
 * rocm_ernic_compat.h), and the count becomes the guest's max_srq verbatim.
 * The admin queue's map is scanned linearly, so a much larger value wants
 * an indexed lookup instead; the ids are dense, so indexing is easy when it
 * is needed.
 */
#define ERNIC_MAX_SRQ 256u

/*
 * Address handles. The guest driver picks their ids, below this, and they
 * index the data path's table; the admin queue keeps no map of them, and
 * the resource manager no table.
 */
#define ERNIC_MAX_AH (1u << 15)

/*
 * Memory regions. Each one the guest creates takes a slot in the resource
 * manager's table, the admin queue's map and the data path's, and the data
 * path searches its table on every fast registration -- every NVMe command
 * on the NVMe-oF path -- so this is also a per-I/O cost. 2048 covers the
 * regions an nvme-rdma initiator registers for its default 8 queues with
 * room to spare; see IONIC_MR_PER_QUEUE in ionic_datapath.h.
 *
 * The driver never hands out MR index 0, the reserved lkey, so the LIF
 * identity advertises one more than this for the guest to be able to create
 * this many.
 */
#define ERNIC_MAX_MR 2048u

/*
 * SGEs in one work request: the most the data path reads from a WQE. The
 * guest driver works out its own limit from the largest WQE stride the LIF
 * identity allows, and ionic_datapath.c checks that this many fill the
 * largest WQE.
 */
#define ERNIC_MAX_SGE 32u

/*
 * SGEs in one SRQ receive, which CREATE_SRQ reports. Mirrors
 * IONIC_MAX_SRQ_SGES in the guest driver's ionic_fw.h, which refuses a
 * create asking for more.
 */
#define ERNIC_MAX_SRQ_SGE 2u

#endif /* ERNIC_LIMITS_H */
