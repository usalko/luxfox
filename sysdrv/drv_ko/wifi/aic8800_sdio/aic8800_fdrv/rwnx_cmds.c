#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
#include <net/mac80211.h>
#endif

#include "aicwf_usb.h"

/**
 * rwnx_send_set_monitor_mode_req - send set monitor mode request
 * @rwnx_hw: rwnx hardware
 * @freq: frequency
 * @fcsfail: fcsfail
 * @control: control
 * @other_bss: other_bss
 * @promisc: promisc
 *
 * Return: 0 on success, -ENOMEM on failure
 */
int rwnx_send_set_monitor_mode_req(struct rwnx_hw *rwnx_hw, int freq, bool fcsfail, bool control, bool other_bss, bool promisc)
{
	struct rwnx_cmd *cmd;
	struct set_monitor_mode_req *req;
	int ret;

	cmd = rwnx_cmd_alloc(rwnx_hw, RWNX_CMD_FLAG_NONBLOCK, ME_SET_MONITOR_MODE_REQ,
						 sizeof(struct set_monitor_mode_req), (void **)&req);
	if (!cmd)
		return -ENOMEM;

	req->freq = freq;
	req->fcsfail = fcsfail;
	req->control = control;
	req->other_bss = other_bss;
	req->promisc = promisc;

	ret = rwnx_hw->cmd_mgr.queue(&rwnx_hw->cmd_mgr, cmd);
	if (ret)
		printk("set monitor mode fail\n");

	return ret;
}