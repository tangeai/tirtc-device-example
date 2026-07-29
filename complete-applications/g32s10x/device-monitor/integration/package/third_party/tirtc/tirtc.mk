ifeq ($(CONFIG_SOC_G32S10X),y)
TIRTC_ROOT := third_party/tirtc
TIRTC_INC_DIR := $(TIRTC_ROOT)/include
TIRTC_LIB_DIR ?= $(TIRTC_ROOT)/lib/g32

CFLAGS += -I$(TIRTC_INC_DIR)
CFLAGS += -I$(TIRTC_INC_DIR)/TiRTC
CFLAGS += -I$(TIRTC_INC_DIR)/platform

package_lib-$(CONFIG_TIRTC) += $(TIRTC_LIB_DIR)/libTiRTC.a
else
$(error TiRTC adaptation currently only supports CONFIG_SOC_G32S10X)
endif

#-------------------------------------------------------
package_name = tirtc
package_depends = mbedtls
package_builtin_src =
package_make_hook =
package_init_hook =
package_finalize_hook =
package_clean_hook =
#-------------------------------------------------------
