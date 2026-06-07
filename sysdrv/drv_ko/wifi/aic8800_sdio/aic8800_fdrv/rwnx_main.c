#define AIC_DRIVER_VERSION "AIC_DRIVER_VERSION_20240606_2"

/**
 * @brief Initialize the cfg80211 interface
 *
 * @param rwnx_hw: Main driver data
 */
int rwnx_cfg80211_init(struct rwnx_hw *rwnx_hw)
{
    struct wiphy *wiphy = rwnx_hw->wiphy;
    struct rwnx_mod_params *mod_params = rwnx_hw->mod_params;
    int i, n_cipher;

    printk("%s\n", AIC_DRIVER_VERSION);

#ifdef CONFIG_RWNX_FULLMAC
    wiphy->flags |= WIPHY_FLAG_HAS_REMAIN_ON_CHANNEL;
    wiphy->flags |= WIPHY_FLAG_OFFCHAN_TX;
#endif

    rwnx_hw->wiphy->max_scan_ssids = rwnx_hw->mod_params->max_scan_ssids;
    rwnx_hw->wiphy->max_scan_ie_len = rwnx_hw->mod_params->max_scan_ie_len;
#if defined(CONFIG_MONITOR_INTERFACE)
    rwnx_hw->wiphy->iface_combinations = rwnx_combinations_ext;
    rwnx_hw->wiphy->n_iface_combinations = ARRAY_SIZE(rwnx_combinations_ext);
#else
    rwnx_hw->wiphy->iface_combinations = rwnx_combinations;
    rwnx_hw->wiphy->n_iface_combinations = ARRAY_SIZE(rwnx_combinations);
#endif
    rwnx_hw->wiphy->flags |= WIPHY_FLAG_SUPPORTS_SCHED_SCAN;
    rwnx_hw->wiphy->flags |= WIPHY_FLAG_IBSS_RSN;
    rwnx_hw->wiphy->flags |= WIPHY_FLAG_AP_UAPSD;

    return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 7, 0)
#ifndef CONFIG_USE_P2P0
    { .max = 1,
      .types = BIT(NL80211_IFTYPE_P2P_DEVICE),
    },
#endif
#endif
    { .max = 1,
      .types = BIT(NL80211_IFTYPE_MONITOR),
    }
};

static struct ieee80211_iface_limit rwnx_limits_dfs[] = {
    {
        .max_interfaces         = NX_VIRT_DEV_MAX,
    },
    /* Keep this combination as the last one */
    {
        .max_interfaces         = NX_VIRT_DEV_MAX,
    },
};

static const struct ieee80211_iface_combination rwnx_combinations_ext[] = {
    {
        .limits                 = rwnx_limits,
        .n_limits               = ARRAY_SIZE(rwnx_limits),
        .num_different_channels = NX_CHAN_CTXT_CNT,
        .max_interfaces         = NX_VIRT_DEV_MAX,
    },
    {
        .limits = (const struct ieee80211_iface_limit[]) {
            { .max = 1, .types = BIT(NL80211_IFTYPE_MONITOR) },
            { .max = 1, .types = BIT(NL80211_IFTYPE_STATION) },
        },
        .n_limits = 2,
        .num_different_channels = 1,
        .max_interfaces = 2,
    },
    {
        .limits = (const struct ieee80211_iface_limit[]) {
            { .max = 1, .types = BIT(NL80211_IFTYPE_MONITOR) },
            { .max = 1, .types = BIT(NL80211_IFTYPE_AP) },
        },
        .n_limits = 2,
        .num_different_channels = 1,
        .max_interfaces = 2,
    },
};

/* There isn't a lot of sense in it, but you can transmit anything you like */
static struct ieee80211_txrx_stypes
rwnx_default_mgmt_stypes[NUM_NL80211_IFTYPES] = {
    [NL80211_IFTYPE_STATION] = {
        .tx = 0xffff,
        .rx = 0xffff,
    },
    [NL80211_IFTYPE_AP] = {
        .tx = 0xffff,
        .rx = 0xffff,
    },
    [NL80211_IFTYPE_MONITOR] = {
        .tx = 0xffff,
        .rx = 0xffff,
    },
};


static u32 cipher_suites[] = {
    [WPA_CIPHER_NONE] = 0,
    [WPA_CIPHER_WPA] = 0,
    [WPA_CIPHER_WPA2] = 0,
    [WPA_CIPHER_WPA2PSK] = 0,
    [WPA_CIPHER_WPA3] = 0,
    [WPA_CIPHER_WPA3PSK] = 0,
    [WPA_CIPHER_WPA3SAE] = 0,
};

static struct rwnx_mod_params rwnx_mod_params = {
    .max_scan_ssids = 10,
    .max_scan_ie_len = 256,
};

static struct rwnx_hw rwnx_hw = {
    .wiphy = {
        .max_scan_ssids = rwnx_mod_params.max_scan_ssids,
        .max_scan_ie_len = rwnx_mod_params.max_scan_ie_len,
        .iface_combinations = rwnx_combinations_ext,
        .n_iface_combinations = ARRAY_SIZE(rwnx_combinations_ext),
        .flags = WIPHY_FLAG_SUPPORTS_SCHED_SCAN | WIPHY_FLAG_IBSS_RSN | WIPHY_FLAG_AP_UAPSD,
    },
    .mod_params = &rwnx_mod_params,
};

static struct rwnx_hw *rwnx_hw_ptr = &rwnx_hw;

int rwnx_cfg80211_init(struct rwnx_hw *rwnx_hw)
{
    struct wiphy *wiphy = rwnx_hw->wiphy;
    struct rwnx_mod_params *mod_params = rwnx_hw->mod_params;
    int i, n_cipher;

    printk("%s\n", AIC_DRIVER_VERSION);

#ifdef CONFIG_RWNX_FULLMAC
    wiphy->flags |= WIPHY_FLAG_HAS_REMAIN_ON_CHANNEL;
    wiphy->flags |= WIPHY_FLAG_OFFCHAN_TX;
#endif

    rwnx_hw->wiphy->max_scan_ssids = rwnx_hw->mod_params->max_scan_ssids;
    rwnx_hw->wiphy->max_scan_ie_len = rwnx_hw->mod_params->max_scan_ie_len;
#if defined(CONFIG_MONITOR_INTERFACE)
    rwnx_hw->wiphy->iface_combinations = rwnx_combinations_ext;
    rwnx_hw->wiphy->n_iface_combinations = ARRAY_SIZE(rwnx_combinations_ext);
#else
    rwnx_hw->wiphy->iface_combinations = rwnx_combinations;
    rwnx_hw->wiphy->n_iface_combinations = ARRAY_SIZE(rwnx_combinations);
#endif
    rwnx_hw->wiphy->flags |= WIPHY_FLAG_SUPPORTS_SCHED_SCAN;
    rwnx_hw->wiphy->flags |= WIPHY_FLAG_IBSS_RSN;
    rwnx_hw->wiphy->flags |= WIPHY_FLAG_AP_UAPSD;

    return 0;
}