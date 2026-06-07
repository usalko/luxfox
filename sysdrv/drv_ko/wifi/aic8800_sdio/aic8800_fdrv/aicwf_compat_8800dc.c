#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
#include <net/mac80211.h>
#endif

int aicwf_set_rf_config_8800dc(struct rwnx_hw *rwnx_hw, struct mm_set_rf_calib_cfm *cfm)
{
	int ret;
	int testmode;

	testmode = cfm->testmode;
	if (testmode == 0) {
		if ((ret = rwnx_send_rf_calib_req(rwnx_hw, cfm))) {
			return -1;
		}
	}
	return 0;
}

void aicwf_set_mon_8800dc(struct rwnx_hw *rwnx_hw, int freq, int flags)
{
	struct rwnx_vif *vif = rwnx_hw->vif_table[0];
	if (!vif)
		return;

	if (flags & MONITOR_FLAG_COOK_FRAMES) {
		printk("AIC8800DC not support COOK_FRAMES\n");
		return;
	}

	rwnx_send_set_monitor_mode_req(rwnx_hw, freq, flags & MONITOR_FLAG_FCSFAIL, flags & MONITOR_FLAG_CONTROL,
		flags & MONITOR_FLAG_OTHER_BSS, flags & MONITOR_FLAG_ACTIVE);
}

int	rwnx_plat_userconfig_load_8800dc(struct rwnx_hw *rwnx_hw)
{
	int size;
	char *buf;
	char *p;
	char *end;
	char *name;
	char *value;
	char *tmp;
	int ret;

	size = rwnx_get_userconfig_size(rwnx_hw);
	if (!size)
		return -1;

	buf = kmalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	p = buf;
	end = buf + size;
	name = p;
	value = p;
	tmp = p;

	while (p < end) {
		if (*p == '\0')
			break;

		if (*p == '#') {
			p++;
			continue;
		}

		if (*p == ' ') {
			p++;
			continue;
		}

		if (*p == '\n') {
			p++;
			continue;
		}

		if (*p == '\t') {
			p++;
			continue;
		}

		if (*p == '\r') {
			p++;
			continue;
		}

		if (*p == '\v') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;
		}

		if (*p == '\u') {
			p++;
			continue;
		}

		if (*p == '\w') {
			p++;
			continue;
		}

		if (*p == '\d') {
			p++;
			continue;
		}

		if (*p == '\c') {
			p++;
			continue;
		}

		if (*p == '\m') {
			p++;
			continue;
		}

		if (*p == '\s') {
			p++;
			continue;
		}

		if (*p == '\h') {
			p++;
			continue;
		}

		if (*p == '\g') {
			p++;
			continue;
		}

		if (*p == '\f') {
			p++;
			continue;
		}

		if (*p == '\b') {
			p++;
			continue;
		}

		if (*p == '\a') {
			p++;
			continue;
		}

		if (*p == '\e') {
			p++;
			continue;