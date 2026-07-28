ATBM_SRC_DIR=devices/wireless/atbm/

CFLAGS += -DATBM_IRQ_USE_GPIO_1=1

CFLAGS += \
	-I shell \
	-I include/os \
	-I include/driver \
	-I os/freertos/include \
	-I net/lwip/include \
	-I net/lwip/include/compat/posix \
	-I $(ATBM_SRC_DIR)api \
	-I $(ATBM_SRC_DIR)hal/compress \
	-I $(ATBM_SRC_DIR)hal/include \
	-I $(ATBM_SRC_DIR)hal/sdio \
	-I $(ATBM_SRC_DIR)hal/spi \
	-I $(ATBM_SRC_DIR)include \
	-I $(ATBM_SRC_DIR)net/include \
	-I $(ATBM_SRC_DIR)net/include/proto \
	-I $(ATBM_SRC_DIR)net/wpa/wps/include \
	-I $(ATBM_SRC_DIR)net/wpa/crypto/include \
	-I $(ATBM_SRC_DIR)net/wpa/sae/include \
	-I $(ATBM_SRC_DIR)net/wpa/p2p/include \
	-I $(ATBM_SRC_DIR)os/include \
	-I $(ATBM_SRC_DIR)os/Ingenic/include

# The vendor driver has no compile-time split between STA and its WPA/WPS/P2P
# internals. Keep that coupled WiFi core, but omit the independent NimBLE host,
# shell, test, iperf and AP DHCP server from this STA-only example.
CFLAGS += -w

WIFI_LIB_COMM = \
	api/atbm_api.c \
	hal/atbm_etf.c \
	hal/atbm_main.c \
	hal/atbm_config.c \
	hal/atbm_timer.c \
	hal/atbm_wsm.c \
	hal/atbm_queue.c \
	hal/sdio/atbm_sdio_bh.c \
	hal/sdio/atbm_sdio_hwio.c \
	hal/sdio/atbm_sdio_fwio.c \
	hal/sdio/atbm_dcxo_dpll.c \
	hal/sdio/atbm_sdio.c \
	hal/atbm_ble_driver_api.c \
	hal/atbm_ble.c \
	hal/atbm_skbuf.c \
	hal/atbm_special.c \
	hal/atbm_sta.c \
	hal/atbm_ap.c \
	hal/atbm_txrx.c \
	hal/atbm_phy_reg.c \
	hal/atbm_init.c \
	hal/smartconfig.c \
	hal/atbm_task.c \
	hal/atbm_thread.c \
	hal/atbm_wifi_driver_api.c \
	net/atbm_mgmt.c \
	net/atbm_rc80211_pid_algo.c \
	net/wpa/wpa_common.c \
	net/wpa/wpa_debug.c \
	net/wpa/hostapd_main.c \
	net/wpa/wpabuf.c \
	net/wpa/aes_core.c \
	net/wpa/wpa_main.c \
	net/wpa/sha1.c \
	net/wpa/wpa_event.c \
	net/wpa/sae/sae.c \
	net/wpa/sae/pmksa_cache.c \
	net/wpa/sae/ecp_curves.c \
	net/wpa/sae/crypto_mbedlts.c \
	net/wpa/sae/dragonfly.c \
	net/wpa/sae/ecp.c \
	net/wpa/sae/ecp_extend.c \
	net/wpa/crypto/sha256.c \
	net/wpa/crypto/dh_group5.c \
	net/wpa/crypto/sha256-internal.c \
	net/wpa/crypto/crypto_internal-modexp.c \
	net/wpa/crypto/dh_groups.c \
	net/wpa/crypto/bignum.c \
	net/wpa/crypto/base64.c \
	net/wpa/crypto/aes-cbc.c \
	net/atbm_rc80211_minstrel.c \
	net/atbm_rc80211_minstrel_ht.c \
	net/atbm_agg.c \
	net/atbm_connect.c \
	net/atbm_bridge.c \
	net/atbm_util.c \
	net/atbm_ratectrl.c \
	net/atbm_key.c \
	os/Ingenic/atbm_os_timer.c \
	os/Ingenic/user/wifi_atbm_api.c \
	os/Ingenic/sdio/atbm_os_sdio.c \
	os/Ingenic/atbm_os_atomic.c \
	os/Ingenic/atbm_os_mutex.c \
	os/Ingenic/atbm_os_msgQ.c \
	os/Ingenic/atbm_os_mem.c \
	os/Ingenic/atbm_os_skbuf.c \
	os/Ingenic/atbm_os_api.c \
	os/Ingenic/atbm_os_thread.c \
	minimal_feature_stubs.c \
	atbm_device_power_control.c \
	atbm_device.c

WIFI_LIB_COMM += \
	net/wpa/p2p/p2p_build.c \
	net/wpa/p2p/p2p_common.c \
	net/wpa/p2p/p2p_dev_disc.c \
	net/wpa/p2p/p2p_go_neg.c \
	net/wpa/p2p/p2p_group.c \
	net/wpa/p2p/p2p_invitation.c \
	net/wpa/p2p/p2p_main.c \
	net/wpa/p2p/p2p_parse.c \
	net/wpa/p2p/p2p_pd.c \
	net/wpa/p2p/p2p_sd.c \
	net/wpa/p2p/p2p_utils.c \
	net/wpa/p2p/p2p.c \
	net/wpa/p2p/wifi_display.c

WIFI_LIB_COMM += \
	net/wpa/wps/eap_common.c \
	net/wpa/wps/eap_register.c \
	net/wpa/wps/eap_server_wsc.c \
	net/wpa/wps/ieee802_11_common.c \
	net/wpa/wps/uuid.c \
	net/wpa/wps/wps_attr_build.c \
	net/wpa/wps/wps_attr_parse.c \
	net/wpa/wps/wps_attr_process.c \
	net/wpa/wps/wps_common.c \
	net/wpa/wps/wps_dev_attr.c \
	net/wpa/wps/wps_enrollee.c \
	net/wpa/wps/wps_hostapd.c \
	net/wpa/wps/wps_main.c \
	net/wpa/wps/wps_registrar.c \
	net/wpa/wps/wps_sta.c \
	net/wpa/wps/wps_validate.c

ATBMWIFI_LIB_SRC = $(WIFI_LIB_COMM)

src-$(CONFIG_WIRELESS_ATBM) += $(ATBMWIFI_LIB_SRC)
