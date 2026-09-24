# V4-only dependencies. The shared components/config/rules tree follows the
# original SDK used by Xiaotai. Do not mix that base package with these headers.
TIRTC_BOARD_COMPAT := $(TOP)/$(TIRTC_DIR)/board_compat
TIRTC_BOARD_BASEPKG := $(TIRTC_BOARD_COMPAT)/basePkg/$(MODEMPKG)
override BASEPKG_MDM_DIR := $(TIRTC_BOARD_BASEPKG)
LD_FILE := $(TIRTC_BOARD_COMPAT)/app.ld
SDK_VERSION_MAJOR := 1
SDK_VERSION_MINOR := 6
SDK_VERSION_PATCH := 04_beta
# Assets are installed explicitly; never embed/erase a filesystem by default.
EF_IMAGE_EN := 0

# Use the matching ABI headers before the older shared SDK include paths.
CFLAGS_INC += -I $(TIRTC_BOARD_COMPAT)/include
CFLAGS_INC += -I $(TIRTC_BOARD_COMPAT)/ecapi/PLAT/prebuild/PLAT/inc
CFLAGS_INC += -I $(TIRTC_BOARD_COMPAT)/ecapi/PLAT/driver/chip/ec7xx/ap/inc
CFLAGS_INC += -I $(TIRTC_BOARD_COMPAT)/ecapi/PLAT/driver/chip/ec7xx/ap/inc_cmsis
CFLAGS_INC += -I $(TIRTC_BOARD_COMPAT)/ecapi/PLAT/device/target/board/common/ARMCM3/inc
CFLAGS_INC += -I $(TIRTC_BOARD_COMPAT)/ecapi/PLAT/device/target/board/ec7xx_0h00/ap/inc

# Explicit project recipes replace only these two objects in the V4 build.
# Keep the SDK sources unchanged for other projects. The startup replacement
# retains the fix for the original SDK's byte-count/word-count zeroing bug.
$(BUILDDIR)/components/kernel/core/appram.o: $(TIRTC_BOARD_COMPAT)/appram.c
	@mkdir -p $(dir $@)
	$(ECHO) CC $<
	$(Q)$(CC) $(CFLAGS) $(CFLAGS_CPU) $(CFLAGS_INC) $(CFLAGS_DEFS) $(DEPFLAGS) -c $< -o $@

$(BUILDDIR)/components/driver/lcd/src/liot_lcdDev_ST7789.o: $(TIRTC_BOARD_COMPAT)/liot_lcdDev_ST7789.c
	@mkdir -p $(dir $@)
	$(ECHO) CC $<
	$(Q)$(CC) $(CFLAGS) $(CFLAGS_CPU) $(CFLAGS_INC) $(CFLAGS_DEFS) $(DEPFLAGS) -c $< -o $@

# Compile the unchanged shared LVGL sources using the V4 configuration and
# Windows-compatible archive response paths, without changing its Makefile.
BUILD_THPART_LVGL_ENABLE := n

# Preserve the tested archive search order: UI, board drivers, then LVGL.
# This affects only this application, and retains any additional project libs.
override LIBURS = $(addprefix $(BUILDDIR)/lib/libusr/,$(filter-out liblvgl.a,$(libusr-y)) $(filter liblvgl.a,$(libusr-y)))
$(BUILDDIR)/$(BINNAME).elf: $(TIRTC_BOARD_COMPAT)/board_compat.mk $(TIRTC_BOARD_COMPAT)/app.ld
