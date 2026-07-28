#-------------------------------------------------------
package_name = application
package_depends =
package_builtin_src = application/
package_make_hook =
package_init_hook =
package_finalize_hook =
package_clean_hook =
#-------------------------------------------------------

package-$(CONFIG_APPLICATION_TIRTC_WIFI_LINK) += package/application/tirtc_g32s10x_wifi_link_demo/tirtc_wifi_link.mk
