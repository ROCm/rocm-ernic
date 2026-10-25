// SPDX-License-Identifier: GPL-2.0
/* Copyright (C) Advanced Micro Devices, Inc. */
/*
 * ernic_p4 - a control driver for the rocm-ernic P4 pipeline
 *
 * rocm-ernic's emulated ionic NIC offers a P4 match-action pipeline that
 * a guest programs through admin-queue commands 70, 71 and 72. The
 * patched ionic driver creates an "ionic.p4" auxiliary device when the
 * NIC advertises IONIC_LIF_CAP_P4; this binds to it and puts a
 * /dev/ernic-p4 in front of it.
 *
 * It is a pipe, not a policy. The device decides what a guest may do --
 * the operator's grant lives there, behind --p4-guest-scope -- and this
 * driver neither enforces nor second-guesses it. What it does own is the
 * boundary between a userspace pointer and a device DMA address: buffers
 * are bounced through coherent memory this driver allocates, so a page
 * the caller unmaps between the ioctl and the completion is never
 * something the device can reach.
 */

#include <linux/auxiliary_bus.h>
#include <linux/cleanup.h>
#include <linux/dma-mapping.h>
#include <linux/idr.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>

#include <ionic.h>
#include <ionic_api.h>
#include <ionic_lif.h>

#include "uapi/ernic_p4.h"

MODULE_DESCRIPTION("rocm-ernic P4 pipeline control");
MODULE_AUTHOR("Advanced Micro Devices, Inc.");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("NET_IONIC");

/* The device's own opcodes; see src/p4/p4_devcmd.h in rocm-ernic. */
#define IONIC_CMD_P4_IDENT 70
#define IONIC_CMD_P4_RPC 71
#define IONIC_CMD_P4_QUERY 72

/*
 * Numbers the character devices, which is not the same as numbering the
 * auxiliary devices. ionic allocates aux ids from one ida shared with
 * its rdma device, so the first P4 device on a host is ionic.p4.1, not
 * .0 -- naming the node after that id gave /dev/ernic-p4.1 on a machine
 * with exactly one NIC, and everything looking for /dev/ernic-p4 found
 * nothing. What the node is called is this driver's business.
 */
static DEFINE_IDA(ernic_p4_ida);

struct ernic_p4_dev {
	struct ionic_lif *lif;
	int id;
	struct device *hwdev;
	struct miscdevice misc;
	char name[32];
	/* One command at a time. The admin queue would serialise these
	 * anyway, but the bounce buffers below are per-device, and two
	 * callers sharing them would each see the other's bytes.
	 */
	struct mutex lock;
	void *req;
	dma_addr_t req_dma;
	void *resp;
	dma_addr_t resp_dma;
};

/*
 * Post one 64-byte command and wait.
 *
 * ionic_adminq_post_wait() returns a negative errno for a transport
 * failure; a command the device refused comes back 0 here with the
 * device's own status in the completion, which is passed up rather than
 * translated. The device's status codes are a closed set that does not
 * match errno on any architecture, so turning them into one here would
 * lose which refusal it was.
 */
static int ernic_p4_post(struct ernic_p4_dev *pdev, const void *cmd64,
			 u32 *status, u32 *resp_len)
{
	struct ionic_admin_ctx ctx;
	int err;

	memset(&ctx, 0, sizeof(ctx));
	init_completion(&ctx.work);
	memcpy(&ctx.cmd, cmd64, sizeof(ctx.cmd));

	err = ionic_adminq_post_wait(pdev->lif, &ctx);
	if (err)
		return err;

	*status = ctx.comp.comp.status;

	/*
	 * The device's RPC completion puts the length it wrote at bytes
	 * 4-7, which fall inside struct ionic_admin_comp's cmd_data[] and
	 * so survive the fields the transport owns: status at 0,
	 * comp_index at 2-3 and color at 15.
	 */
	if (resp_len)
		*resp_len = get_unaligned_le32((u8 *)&ctx.comp + 4);
	return 0;
}

static int ernic_p4_ident(struct ernic_p4_dev *pdev, unsigned long arg)
{
	struct ernic_p4_ident req;
	u8 cmd[64] = {};
	u32 status = 0;
	int err;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (!req.len || req.len > ERNIC_P4_RESP_MAX)
		return -EINVAL;

	guard(mutex)(&pdev->lock);

	memset(pdev->resp, 0, ERNIC_P4_RESP_MAX);

	cmd[0] = IONIC_CMD_P4_IDENT;
	/* cmd[1] is the ABI version this driver speaks; 0 today. Bytes
	 * 2-3 are lif_index, as in every other ionic admin command.
	 */
	put_unaligned_le16(pdev->lif->index, cmd + 2);
	put_unaligned_le32(req.len, cmd + 4);
	put_unaligned_le64(pdev->resp_dma, cmd + 8);

	err = ernic_p4_post(pdev, cmd, &status, NULL);
	if (err)
		return err;

	req.status = status;
	if (!status &&
	    copy_to_user(u64_to_user_ptr(req.buf), pdev->resp, req.len))
		return -EFAULT;
	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;
	return 0;
}

static int ernic_p4_query(struct ernic_p4_dev *pdev, unsigned long arg)
{
	struct ernic_p4_query req;
	u8 cmd[64] = {};
	u32 status = 0;
	int err;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (!req.len || req.len > ERNIC_P4_RESP_MAX)
		return -EINVAL;
	if (req.entity > U8_MAX)
		return -EINVAL;

	guard(mutex)(&pdev->lock);

	memset(pdev->resp, 0, ERNIC_P4_RESP_MAX);

	cmd[0] = IONIC_CMD_P4_QUERY;
	cmd[1] = (u8)req.entity;
	put_unaligned_le16(pdev->lif->index, cmd + 2);
	put_unaligned_le32(req.len, cmd + 4);
	put_unaligned_le64(pdev->resp_dma, cmd + 8);
	put_unaligned_le32(req.ep, cmd + 16);

	err = ernic_p4_post(pdev, cmd, &status, NULL);
	if (err)
		return err;

	req.status = status;
	if (!status &&
	    copy_to_user(u64_to_user_ptr(req.buf), pdev->resp, req.len))
		return -EFAULT;
	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;
	return 0;
}

static int ernic_p4_rpc(struct ernic_p4_dev *pdev, unsigned long arg)
{
	struct ernic_p4_rpc req;
	u8 cmd[64] = {};
	u32 status = 0;
	u32 written;
	int err;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (req.req_len > ERNIC_P4_REQ_MAX || req.resp_len > ERNIC_P4_RESP_MAX)
		return -EINVAL;
	if (req.scope > U8_MAX)
		return -EINVAL;

	guard(mutex)(&pdev->lock);

	/* Cleared even when the caller supplies the whole thing: a short
	 * request must not leave the previous caller's bytes where the
	 * device can read them as this one's.
	 */
	memset(pdev->req, 0, ERNIC_P4_REQ_MAX);
	memset(pdev->resp, 0, ERNIC_P4_RESP_MAX);

	if (req.req_len &&
	    copy_from_user(pdev->req, u64_to_user_ptr(req.req), req.req_len))
		return -EFAULT;

	cmd[0] = IONIC_CMD_P4_RPC;
	cmd[1] = (u8)req.scope;
	put_unaligned_le16(pdev->lif->index, cmd + 2);
	put_unaligned_le32(req.ep, cmd + 4);
	put_unaligned_le32(req.op, cmd + 8);
	put_unaligned_le32(req.req_len, cmd + 12);
	put_unaligned_le64(req.req_len ? pdev->req_dma : 0, cmd + 16);
	put_unaligned_le32(req.resp_len, cmd + 24);
	put_unaligned_le64(req.resp_len ? pdev->resp_dma : 0, cmd + 32);

	err = ernic_p4_post(pdev, cmd, &status, &written);
	if (err)
		return err;

	req.status = status;

	/*
	 * The device reports how much it wrote. Trust it only as far as
	 * the buffer the caller offered: a device reporting more than it
	 * was given would otherwise copy past the end of the bounce
	 * buffer, which is the one thing a driver on this boundary must
	 * not let a device talk it into.
	 */
	if (status || !req.resp_len)
		written = 0;
	written = min(written, req.resp_len);
	if (written &&
	    copy_to_user(u64_to_user_ptr(req.resp), pdev->resp, written))
		return -EFAULT;
	req.resp_len = written;

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;
	return 0;
}

static long ernic_p4_unlocked_ioctl(struct file *file, unsigned int cmd,
				    unsigned long arg)
{
	struct ernic_p4_dev *pdev =
		container_of(file->private_data, struct ernic_p4_dev, misc);

	switch (cmd) {
	case ERNIC_P4_IOC_IDENT:
		return ernic_p4_ident(pdev, arg);
	case ERNIC_P4_IOC_QUERY:
		return ernic_p4_query(pdev, arg);
	case ERNIC_P4_IOC_RPC:
		return ernic_p4_rpc(pdev, arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations ernic_p4_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = ernic_p4_unlocked_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static int ernic_p4_probe(struct auxiliary_device *adev,
			  const struct auxiliary_device_id *id)
{
	struct ionic_aux_dev *ionic_adev =
		container_of(adev, struct ionic_aux_dev, adev);
	struct ernic_p4_dev *pdev;
	int err;

	pdev = devm_kzalloc(&adev->dev, sizeof(*pdev), GFP_KERNEL);
	if (!pdev)
		return -ENOMEM;

	pdev->lif = ionic_adev->lif;
	pdev->hwdev = &pdev->lif->ionic->pdev->dev;
	mutex_init(&pdev->lock);

	pdev->req = dmam_alloc_coherent(pdev->hwdev, ERNIC_P4_REQ_MAX,
					&pdev->req_dma, GFP_KERNEL);
	if (!pdev->req)
		return -ENOMEM;

	pdev->resp = dmam_alloc_coherent(pdev->hwdev, ERNIC_P4_RESP_MAX,
					 &pdev->resp_dma, GFP_KERNEL);
	if (!pdev->resp)
		return -ENOMEM;

	/*
	 * One node per device rather than a fixed name: a host running
	 * more than one emulated NIC would otherwise have the second
	 * probe fail on a name already taken, and the operator would have
	 * no way to say which device they meant. The first is plain
	 * /dev/ernic-p4 whatever the auxiliary bus numbered it.
	 */
	pdev->id = ida_alloc(&ernic_p4_ida, GFP_KERNEL);
	if (pdev->id < 0)
		return pdev->id;

	if (pdev->id == 0)
		strscpy(pdev->name, "ernic-p4", sizeof(pdev->name));
	else
		snprintf(pdev->name, sizeof(pdev->name), "ernic-p4.%d",
			 pdev->id);

	pdev->misc.minor = MISC_DYNAMIC_MINOR;
	pdev->misc.name = pdev->name;
	pdev->misc.fops = &ernic_p4_fops;
	pdev->misc.parent = &adev->dev;
	/* Owner-only. The engine decides where every frame goes, so the
	 * node is not something to leave open to a group by default.
	 */
	pdev->misc.mode = 0600;

	err = misc_register(&pdev->misc);
	if (err) {
		dev_err(&adev->dev, "failed to register /dev/%s: %d\n",
			pdev->name, err);
		ida_free(&ernic_p4_ida, pdev->id);
		return err;
	}

	auxiliary_set_drvdata(adev, pdev);
	dev_info(&adev->dev, "P4 control surface at /dev/%s\n", pdev->name);
	return 0;
}

static void ernic_p4_remove(struct auxiliary_device *adev)
{
	struct ernic_p4_dev *pdev = auxiliary_get_drvdata(adev);

	misc_deregister(&pdev->misc);
	ida_free(&ernic_p4_ida, pdev->id);
}

static const struct auxiliary_device_id ernic_p4_id_table[] = {
	{
		.name = "ionic.p4",
	},
	{},
};
MODULE_DEVICE_TABLE(auxiliary, ernic_p4_id_table);

static struct auxiliary_driver ernic_p4_driver = {
	.name = "ernic_p4",
	.probe = ernic_p4_probe,
	.remove = ernic_p4_remove,
	.id_table = ernic_p4_id_table,
};

module_auxiliary_driver(ernic_p4_driver);
