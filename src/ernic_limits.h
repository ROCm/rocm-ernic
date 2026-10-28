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
 * ionic/ionic_datapath.h), and the count becomes the guest's max_srq
 * verbatim. The admin queue's map is scanned linearly, so a much larger
 * value wants an indexed lookup instead; the ids are dense, so indexing is
 * easy when it is needed.
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
 * manager's table, and the slot its index names in the admin queue's map
 * and the data path's table (ionic_mr_slot()). 2048 covers the regions an
 * nvme-rdma initiator registers for its default 8 queues with room to
 * spare; see IONIC_MR_PER_QUEUE in ionic_datapath_nvmeof.h.
 *
 * The driver never hands out MR index 0, the reserved lkey, so the LIF
 * identity advertises one more than this for the guest to be able to create
 * this many, under indices 1 to this.
 */
#define ERNIC_MAX_MR 2048u

/*
 * Page table entries, the LIF identity's npts_per_lif: how many the device's
 * page tables hold between them. No one table can need more, so the data
 * path refuses a ring or memory region whose table claims more
 * (ionic_dp_buf_init()), whatever length the guest gives the region; that
 * length is the guest's own choice, and bounds nothing on its own.
 *
 * This is also the knob that sets the guest's maximum memory-region size,
 * and that is not obvious from here, so: the driver does
 *
 *   attr->max_mr_size = dev->lif_cfg.npts_per_lif * PAGE_SIZE / 2;
 *   (drivers/infiniband/hw/ionic/ionic_ibdev.c:37)
 *
 * -- an explicit halving, presumably for page-table headroom. At the previous
 * (1u << 20) that reported 1048576 * 4096 / 2 = 0x80000000, i.e. 2 GiB, which
 * reads like something truncating a 4 GiB constant and is not: it is this
 * formula working as intended. 1u << 21 reports 4 GiB.
 *
 * Nothing is allocated from this value on either side -- the device builds
 * each table as its region is created, at the size the guest asks for, and
 * the driver uses it only for max_mr_size and max_fast_reg_page_list_len
 * (ionic_ibdev.c:37,74) -- so raising it costs no memory up front. It does
 * let one table grow larger: 8 bytes an entry, so 16 GiB at this value.
 *
 * It matters because LMCache registers its whole L1 arena as ONE memory
 * region (nixl_impl.py:341), so max_mr_size is a hard ceiling on per-node L1.
 *
 * WHY SO LARGE. Real HCAs advertise max_mr_size as effectively unbounded
 * (ConnectX-class parts report UINT64_MAX) and let pinnable host memory be
 * the real limit. A small advertised cap is an emulation artifact that
 * surfaces as a mystifying ENOMEM from ibv_reg_mr long before the host is
 * anywhere near out of memory, so match the hardware convention and let
 * memory do the limiting.
 *
 * 1u << 31 reports 2^31 * 4096 / 2 = 2^42 = 4 TiB. The arithmetic is safe:
 * npts_per_lif is u32 and PAGE_SIZE is unsigned long, so the product is done
 * in 64 bits, and max_fast_reg_page_list_len (npts/2 = 2^30) still fits the
 * u32 it is assigned to. A literal 0xFFFFFFFF is avoided deliberately -- it
 * would be ~8 TiB but reads like a sentinel.
 *
 * The max_mr_size the TCP and loopback backends' query_device() report
 * (0xFFFFFFFF) does NOT constrain this: it lands only in the pvrdma dev_attr,
 * which the ionic guest driver never reads, and no registration path
 * validates against it.
 */
#define ERNIC_MAX_PAGE_TABLE_ENTRIES (1u << 31)

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
