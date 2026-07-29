#main
ifeq ($(CONFIG_APPLICATION_SCANCODE),y)
CFLAGS += -I$(TOPDIR)application/app_scancode/ui/include
endif

ifeq ($(CONFIG_APPLICATION_TIRTC_DEMO),y)
CFLAGS += -I$(TOPDIR)application/app_tirtc_demo/include
endif

src-$(CONFIG_APPLICATION_HOME) += ui_main.c
