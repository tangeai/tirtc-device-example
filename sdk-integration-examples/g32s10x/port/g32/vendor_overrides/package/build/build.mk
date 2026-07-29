BUILD_CFLAGS += -isystem $(shell $(CC) -print-file-name=include)
BUILD_CFLAGS += -isystem $(shell $(CC) -print-file-name=include-fixed)
BUILD_CFLAGS += -fno-builtin -ffreestanding -nostdinc -nostdlib
BUILD_CFLAGS += -I$(TOPDIR)include -I$(TOPDIR)
BUILD_CFLAGS += -std=gnu99 -Wno-enum-compare -fno-strict-aliasing -Wno-format-zero-length
# The r1.0.5 ABI typedefs uint32_t as unsigned long, while legacy vendor
# diagnostics use %x/%d. Application sources are still checked with
# -Wformat=2 -Werror by scripts/syntax_check.sh.
BUILD_CFLAGS += -Wno-format
BUILD_CFLAGS += -Wall -Wstrict-prototypes -fno-stack-protector
BUILD_CFLAGS += $(CONFIG_OS_GCC_OPTIMIZATION)

MODULE_CFLAGS += $(BUILD_CFLAGS) -fno-common
CFLAGS += $(BUILD_CFLAGS) -ffunction-sections -fdata-sections

LDFLAGS += $(CONFIG_OS_GCC_OPTIMIZATION)

ASM_FLAGS += -D__ASSEMBLY__

ifneq ($(CONFIG_TCSM_SECTION),y)
CONFIG_TCSM_SECTION_ADDR:=$(CONFIG_OS_MEM_ADDR)
CONFIG_TCSM_SECTION_SIZE:=$(CONFIG_OS_MEM_SIZE)
endif

define build_init_hook
	$(Q)echo $(MSG_LDS) build/build.lds.in
	$(Q)echo "main_stack_size = $(CONFIG_OS_MAIN_STACK_SIZE);" > build/build.lds.in
	$(Q)echo "exception_stack_size = $(CONFIG_OS_EXCEPTION_STACK_SIZE);" >> build/build.lds.in
	$(Q)echo "exception_section_size = $(CONFIG_OS_EXCEPTION_SECTION_SIZE);" >> build/build.lds.in
	$(Q)echo "sram_start = $(CONFIG_OS_MEM_ADDR);" >> build/build.lds.in
	$(Q)echo "sram_size = $(CONFIG_OS_MEM_SIZE);" >> build/build.lds.in
	$(Q)echo "tcsm_start = $(CONFIG_TCSM_SECTION_ADDR);" >> build/build.lds.in
	$(Q)echo "tcsm_size = $(CONFIG_TCSM_SECTION_SIZE);" >> build/build.lds.in
	$(Q)echo "MEMORY { .sram : ORIGIN = sram_start, LENGTH = sram_size }" >> build/build.lds.in
	$(Q)echo "MEMORY { .tcsm : ORIGIN = tcsm_start, LENGTH = tcsm_size }" >> build/build.lds.in
endef

define build_clean_hook
	$(Q)echo $(MSG_CLEAN) build/build.lds.in
	$(Q)rm -f build/build.lds.in
endef

#-------------------------------------------------------
package_name = build
package_depends =
package_builtin_src =
package_make_hook =
package_init_hook = build_init_hook
package_finalize_hook =
package_clean_hook = build_clean_hook
#-------------------------------------------------------
