CFLAGS += -Iapplication/tirtc_g32s10x_wifi_link_demo/include
CFLAGS += -Iapplication/tirtc_g32s10x_wifi_link_demo/sdk/include
CFLAGS += -Iapplication/tirtc_g32s10x_wifi_link_demo/sdk/include/TiRTC
CFLAGS += -Iapplication/tirtc_g32s10x_wifi_link_demo/sdk/include/platform

#-------------------------------------------------------
package_name = tirtc_g32s10x_wifi_link_demo
package_depends =
package_builtin_src = application/tirtc_g32s10x_wifi_link_demo/
package_make_hook =
package_init_hook =
package_finalize_hook =
package_clean_hook =
#-------------------------------------------------------

package_lib-y += application/tirtc_g32s10x_wifi_link_demo/sdk/lib/g32/libTiRTC.a
