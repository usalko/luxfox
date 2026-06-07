int rwnx_send_txpwr_lvl_req(struct rwnx_hw *rwnx_hw);
int rwnx_send_txpwr_lvl_v3_req(struct rwnx_hw *rwnx_hw);
int rwnx_send_set_monitor_mode_req(struct rwnx_hw *rwnx_hw, int freq, bool fcsfail, bool control, bool other_bss, bool promisc);

#ifdef CONFIG_USB_BT
int rwnx_send_reboot(struct rwnx_hw *rwnx_hw);
#endif