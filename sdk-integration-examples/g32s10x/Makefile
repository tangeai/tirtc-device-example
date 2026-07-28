ifeq ($(CONFIG_APPLICATION_TIRTC_WIFI_LINK),y)
ccflags-y += -Iapplication/tirtc_g32s10x_wifi_link_demo/include
ccflags-y += -Iapplication/tirtc_g32s10x_wifi_link_demo/sdk/include
ccflags-y += -Iapplication/tirtc_g32s10x_wifi_link_demo/sdk/include/TiRTC
ccflags-y += -Iapplication/tirtc_g32s10x_wifi_link_demo/sdk/include/platform
endif

src-$(CONFIG_APPLICATION_TIRTC_WIFI_LINK) += src/tirtc_link.c
src-$(CONFIG_APPLICATION_TIRTC_WIFI_LINK) += src/tirtc_link_platform.c
src-$(CONFIG_APPLICATION_TIRTC_WIFI_LINK) += src/tirtc_test_log.c
src-$(CONFIG_APPLICATION_TIRTC_WIFI_LINK) += src/tirtc_sample_avi.c
src-$(CONFIG_APPLICATION_TIRTC_WIFI_LINK) += src/tirtc_sample_media.c
src-$(CONFIG_APPLICATION_TIRTC_WIFI_LINK) += src/tirtc_sample_asset.S
