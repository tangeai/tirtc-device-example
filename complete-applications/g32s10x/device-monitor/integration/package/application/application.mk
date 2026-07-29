#-------------------------------------------------------
package_name = application
package_depends =
package_builtin_src = application/
package_make_hook =
package_init_hook =
package_finalize_hook =
package_clean_hook =
#-------------------------------------------------------

package-$(CONFIG_APPLICATION_LAUNCHER) += package/application/app_launcher/launcher.mk
package-$(CONFIG_APPLICATION_LOAD_KERNEL) += package/application/load_kernel/load_kernel.mk
package-$(CONFIG_APPLICATION_HOME) += package/application/app_home/home.mk
package-$(CONFIG_APPLICATION_INFRARED_FACE) += package/application/infrared_face/infrared_face.mk
package-$(CONFIG_APPLICATION_IVS_FACE) += package/application/app_ivs_face/ivs_face.mk
package-$(CONFIG_APPLICATION_MULTI_OBJ_DET) += package/application/app_multi_obj/multi_obj.mk
package-$(CONFIG_APPLICATION_SCANCODE) += package/application/app_scancode/scancode.mk
package-$(CONFIG_APPLICATION_AUDIO) += package/application/app_audio/audio.mk
package-$(CONFIG_APPLICATION_TIRTC_DEMO) += package/application/app_tirtc_demo/tirtc_demo.mk
package-$(CONFIG_APPLICATION_SETTING) += package/application/app_setting/setting.mk
package-$(CONFIG_APPLICATION_STATUS_BAR) += package/application/app_status_bar/status_bar.mk
package-$(CONFIG_APPLICATION_SERVICES) += package/application/com_services/com_services.mk
package-$(CONFIG_APPLICATION_MOTOR) += package/application/app_motor/app_motor.mk
package-$(CONFIG_APPLICATION_ALIYUN) += package/application/app_aliyun/aliyun.mk
package-$(CONFIG_APPLICATION_MICROPYTHON) += package/application/app_micropython/micropython.mk
