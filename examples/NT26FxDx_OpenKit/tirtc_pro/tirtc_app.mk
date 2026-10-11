include $(TOP)/$(TIRTC_DIR)/board_compat/board_compat.mk

CFLAGS_INC += -I $(TOP)/$(TIRTC_DIR) -I $(TOP)/$(TIRTC_DIR)/port -I $(TOP)/$(TIRTC_DIR)/ui -I $(TOP)/$(TIRTC_DIR)/network
CFLAGS_INC += -I $(TOP)/$(TIRTC_DIR)/resources -I $(TOP)/$(TIRTC_DIR)/storage
CFLAGS_INC += -I $(TOP)/$(TIRTC_DIR)/status
CFLAGS_INC += -I $(TOP)/$(TIRTC_DIR)/preferences
CFLAGS_INC += -I $(TOP)/$(TIRTC_DIR)/keys
CFLAGS_INC += -I $(TOP)/$(TIRTC_DIR)/platform
CFLAGS_INC += -I $(TOP)/$(TIRTC_DIR)/ai -I $(TOP)/$(TIRTC_DIR)/media -I $(TOP)/$(TIRTC_DIR)/resource_store
CFLAGS_INC += -I $(TOP)/$(TIRTC_DIR)/group
CFLAGS_INC += -I $(TOP)/$(TIRTC_DIR)/contacts -I $(TOP)/$(TIRTC_DIR)/calls -I $(TOP)/$(TIRTC_DIR)/video
CFLAGS_INC += -I $(TOP)/$(TIRTC_DIR)/ui/assets -I $(TOP)/$(TIRTC_DIR)/ui/fonts
CFLAGS_DEFS += -DLV_CONF_PATH=$(TOP)/$(TIRTC_DIR)/ui/lv_conf.h
EXDEMO_LVGL_USE_SOURCE_LIB := y

TIRTC_UI_CFILES := $(wildcard $(TOP)/$(TIRTC_DIR)/ui/*.c) \
                  $(wildcard $(TOP)/$(TIRTC_DIR)/ui/*/*.c) \
                  $(wildcard $(TOP)/$(TIRTC_DIR)/ui/*/*/*.c)
# Storage compiles only wrappers; vendor littlefs is included once by the
# namespaced wrapper, avoiding duplicate SDK lfs_* implementations.
TIRTC_STORAGE_CFILES := $(wildcard $(TOP)/$(TIRTC_DIR)/storage/*.c) \
                       $(TOP)/$(TIRTC_DIR)/storage/examples/tirtc_storage_example.c
TIRTC_NETWORK_CFILES := $(wildcard $(TOP)/$(TIRTC_DIR)/network/*.c)
TIRTC_PLATFORM_CFILES := $(wildcard $(TOP)/$(TIRTC_DIR)/platform/*.c)
TIRTC_CALL_CFILES := $(wildcard $(TOP)/$(TIRTC_DIR)/contacts/*.c) \
                    $(wildcard $(TOP)/$(TIRTC_DIR)/calls/*.c) \
                    $(wildcard $(TOP)/$(TIRTC_DIR)/video/*.c)
ifneq ($(TIRTC_ASSET_INSTALLER),y)
include $(TOP)/$(TIRTC_DIR)/runtime/runtime.mk
CFLAGS_DEFS += -DHWDEMO_GROUP_ROOM_EN
CFLAGS_DEFS += -DTIRTC_EXTERNAL_UI_ASSETS=1
# Keep the stream ABI tied to the official archive tested by Lite. No SDK
# sources are patched; a changed vendor binary requires another contract audit.
ifeq ($(filter y n,$(TIRTC_AUDIO_AEC_EN)),)
$(error TIRTC_AUDIO_AEC_EN must be y or n)
endif
ifeq ($(TIRTC_AUDIO_AEC_EN),y)
TIRTC_AUDIO_SHA256 := $(word 1,$(shell sha256sum "$(TIRTC_BOARD_BASEPKG)/lib/libliot_audio2.a"))
ifneq ($(TIRTC_AUDIO_SHA256),2307921e29b8930f1de61009c9519f0d70c8efe3d1cbfe02d5b4bb86b9ab9cbb)
$(error F6D_D audio library changed; re-audit the AEC stream contracts)
endif
CFLAGS_DEFS += -DDEMO_AI_AUDIO_AEC_ENABLE=1 -DDEMO_AI_AUDIO_AEC_DELAY=$(TIRTC_AUDIO_AEC_DELAY)
CFLAGS_DEFS += -DFEATURE_LIERDA_AUDIO_3A -DFEATURE_SUPPORT_APP_PCM_MEM_POOL
else
CFLAGS_DEFS += -DDEMO_AI_AUDIO_AEC_ENABLE=0
endif
# Command-line backend changes must also rebuild existing product objects.
TIRTC_AEC_STAMP := $(BUILDDIR)/pro_aec_config.h
# The bundled shell has read/test built-ins but does not include cmp.
TIRTC_AEC_STAMP_UPDATE := $(shell mkdir -p "$(BUILDDIR)"; tirtc_aec_previous=''; if test -f "$(TIRTC_AEC_STAMP)"; then IFS= read -r tirtc_aec_previous < "$(TIRTC_AEC_STAMP)"; fi; if test "$$tirtc_aec_previous" != '$(TIRTC_AUDIO_AEC_EN) $(TIRTC_AUDIO_AEC_DELAY)'; then printf '%s\n' '$(TIRTC_AUDIO_AEC_EN) $(TIRTC_AUDIO_AEC_DELAY)' > "$(TIRTC_AEC_STAMP)"; fi)
endif
# The D SDK OS restores the scheduler itself. The remaining binary contracts
# were re-audited against the complete official F6D_D package.
ifeq ($(MODEMPKG),F6D_D)
TIRTC_MQTT_SHA256 := $(word 1,$(shell sha256sum "$(TIRTC_BOARD_BASEPKG)/lib/libliot_mqtts.a"))
ifneq ($(TIRTC_MQTT_SHA256),e2cb8a61ddcca04d63fdc671e293b78454727a064c31d453b2bb06d276a33746)
$(error F6D_D libliot_mqtts.a changed; review asynchronous MQTT cleanup compatibility)
endif
CFLAGS_DEFS += -DTIRTC_MQTT_F6D_ABI_VERIFIED=1

# App-only camera DMA completion fence. The SDK's frame-end IRQ and DMA-end
# callback release the same capture semaphore; wait for the latter explicitly.
# The copied callback table ABI also depends on the base ELF hash below.
TIRTC_CSPI_SHA256 := $(word 1,$(shell sha256sum "$(TIRTC_BOARD_BASEPKG)/lib/libliot_cspi.a"))
TIRTC_CAMERA_SHA256 := $(word 1,$(shell sha256sum "$(TIRTC_BOARD_BASEPKG)/lib/libliot_camera.a"))
TIRTC_CAMERA_HEADER_SHA256 := $(word 1,$(shell sha256sum "$(TOP)/components/kernel/lierda_api/liot_camera/liot_camera.h"))
ifneq ($(TIRTC_CSPI_SHA256),177b27bf03ce11fdac4dcbeafc3dceac7f85ca9078223c24cd09226e128a7c2d)
$(error F6D_D libliot_cspi.a changed; review the camera DMA completion fence)
endif
ifneq ($(TIRTC_CAMERA_SHA256),3cbaaed84d6b967be211781b844f2ab4b83d377f515dee266daf0f34116c0311)
$(error F6D_D libliot_camera.a changed; review the camera DMA completion fence)
endif
ifneq ($(TIRTC_CAMERA_HEADER_SHA256),e173ef09a0539d79bbb5c41537ffd47f9cb0263afb9b9663ca63cde67fd2ee09)
$(error F6D_D liot_camera.h changed; review the camera DMA completion fence ABI)
endif
CFLAGS_DEFS += -DTIRTC_CAMERA_F6D_ABI_VERIFIED=1
ifneq ($(TIRTC_ASSET_INSTALLER),y)
LDFLAGS += -Wl,--wrap=liot_cspi_get_intf
endif

# Video buffer sizes and JPEG DecodeLine ownership are verified against these
# untouched SDK artifacts. A vendor upgrade requires a fresh contract audit.
TIRTC_JPEG_SHA256 := $(word 1,$(shell sha256sum "$(TIRTC_BOARD_BASEPKG)/lib/libmm_jpeg.a"))
TIRTC_VIDEO_SHA256 := $(word 1,$(shell sha256sum "$(TIRTC_BOARD_BASEPKG)/lib/libmm_videoutil.a"))
TIRTC_JPEG_HEADER_SHA256 := $(word 1,$(shell sha256sum "$(TOP)/components/kernel/ecapi/PLAT/prebuild/PLAT/inc/mm_jpeg_if.h"))
ifneq ($(TIRTC_JPEG_SHA256),07979ccc137d323b7e86715abed5ef416748dcb1f43f8d216ca6de192654e032)
$(error F6D_D libmm_jpeg.a changed; review JPEG buffers and DecodeLine ownership)
endif
ifneq ($(TIRTC_VIDEO_SHA256),b534ae2cb5c90d5d29da2550d715ac281997fdbcc429dfca39938ccec758242e)
$(error F6D_D libmm_videoutil.a changed; review video buffer contract)
endif
ifneq ($(TIRTC_JPEG_HEADER_SHA256),5b9b583c78e0eda11935d2eeafc356ac82ed909598050865cb7cc58aeffed644)
$(error F6D_D mm_jpeg_if.h changed; review JPEG API contract)
endif
# The read-only LFS_stat entry preserves NOENT, unlike liot_stat/NVM wrappers.
# Gate its signature/layout/error contract against the audited base package.
TIRTC_PREFS_BASE_SHA256 := $(word 1,$(shell sha256sum "$(TIRTC_BOARD_BASEPKG)/ap_lierda_app.elf"))
TIRTC_PREFS_API_SHA256 := $(word 1,$(shell sha256sum "$(TIRTC_BOARD_BASEPKG)/lib/lib_baseapi.a"))
TIRTC_PREFS_NV_SHA256 := $(word 1,$(shell sha256sum "$(TIRTC_BOARD_BASEPKG)/lib/libliot_nv.a"))
ifneq ($(TIRTC_PREFS_BASE_SHA256),9553cae08079eab350557b86d5474464ffabb3086e7055739c7b57fccc930c3d)
$(error F6D_D base ELF changed; review preference stat error compatibility)
endif
ifneq ($(TIRTC_PREFS_API_SHA256),c5df45aa9df60fdb53fb176a72964f90b84b5900185b80777a3c5991988a00ba)
$(error F6D_D lib_baseapi.a changed; review preference stat ABI)
endif
ifneq ($(TIRTC_PREFS_NV_SHA256),663a0e0fb53b5dd56436d3bf2d4e6f0c3f758434b99a1e7b735e869a209ab766)
$(error F6D_D libliot_nv.a changed; review preference NVM return/path semantics)
endif
CFLAGS_DEFS += -DTIRTC_PREFS_F6D_ABI_VERIFIED=1
else
$(error This standalone board port is verified only against F6D_D)
endif
TIRTC_CFILES := $(TOP)/$(TIRTC_DIR)/user_main.c \
                $(TOP)/$(TIRTC_DIR)/remote/tirtc_remote.c \
                $(wildcard $(TOP)/$(TIRTC_DIR)/group/*.c) \
                $(TOP)/$(TIRTC_DIR)/port/tirtc_port.c \
                $(TIRTC_NETWORK_CFILES) \
                $(TIRTC_PLATFORM_CFILES) \
                $(TIRTC_CALL_CFILES) \
                $(TIRTC_RUNTIME_CFILES) \
                $(TOP)/$(TIRTC_DIR)/ai/tirtc_ai.c \
                $(TOP)/$(TIRTC_DIR)/ai/ai_call_protocol.c \
                $(TOP)/$(TIRTC_DIR)/preferences/tirtc_preferences.c \
                $(TOP)/$(TIRTC_DIR)/keys/tirtc_keys.c \
                $(TOP)/$(TIRTC_DIR)/media/audio_device.c \
                $(TOP)/$(TIRTC_DIR)/media/g711_codec.c \
                $(TOP)/$(TIRTC_DIR)/resource_store/assets.c \
                $(TOP)/$(TIRTC_DIR)/resource_store/asset_io.c \
                $(TOP)/$(TIRTC_DIR)/resource_store/font_cache.c \
                $(TOP)/$(TIRTC_DIR)/resources/tirtc_resources.c \
                $(TOP)/$(TIRTC_DIR)/resources/tirtc_resources_layout.c \
                $(TOP)/$(TIRTC_DIR)/resources/tirtc_resources_workers.c \
                $(TOP)/$(TIRTC_DIR)/status/tirtc_status.c \
                $(TIRTC_STORAGE_CFILES) \
                $(TIRTC_UI_CFILES)
ifeq ($(TIRTC_ASSET_INSTALLER),y)
# Temporary resource installer uses the same base package / APP region.
# It contains original resource bytes, with no RTC or UI application linked.
BUILD_THPART_LVGL_ENABLE := n
BUILD_THPART_CJSON_ENABLE := n
BUILD_THPART_MBEDTLS_ENABLE := n
BUILD_THPART_LWIP_ENABLE := n
BUILD_DRIVER_LCD_EN := n
BUILD_DRIVER_CAMERA_EN := n
BUILD_DRIVER_TP_EN := n
TIRTC_CFILES := $(TIRTC_STORAGE_CFILES) \
                $(TOP)/$(TIRTC_DIR)/resource_store/installer_main.c \
                $(TOP)/$(TIRTC_DIR)/resource_store/asset_installer.c \
                $(TOP)/$(TIRTC_DIR)/resource_store/asset_io.c \
                $(TOP)/$(TIRTC_DIR)/resource_store/installer_blob.c
endif
CUSTOM_COBJSTEMP := $(patsubst $(TOP)/%.c,%.o,$(TIRTC_CFILES))
CUSTOM_COBJS := $(addprefix $(BUILDDIR)/,$(CUSTOM_COBJSTEMP))
PP_FILES += $(CUSTOM_COBJS:.o=.pp)
# Vendor rules do not load .d files, and preprocessing can overwrite them
# with bare targets. Track product headers/includes locally instead; tests
# are host-only. A header change conservatively rebuilds the product objects.
tirtc_pro_files = $(foreach entry,$(wildcard $(1)*),$(if $(filter %/tests,$(entry)),,$(filter $(2),$(entry)) $(call tirtc_pro_files,$(entry)/,$(2))))
TIRTC_PRO_INCLUDES := $(call tirtc_pro_files,$(TOP)/$(TIRTC_DIR)/,%.h %.inc)
$(CUSTOM_COBJS) $(CUSTOM_COBJS:.o=.pp): $(TIRTC_PRO_INCLUDES)
# LVGL also compiles against the product configuration. Its object list is
# declared later by the SDK, so derive these paths without editing SDK rules.
TIRTC_PRO_LVGL_OBJS := $(patsubst $(TOP)/%.c,$(BUILDDIR)/%.o,$(call tirtc_pro_files,$(TOP)/components/thirdparty/lvgl/lvgl-8.3.10/,%.c))
$(TIRTC_PRO_LVGL_OBJS) $(TIRTC_PRO_LVGL_OBJS:.o=.pp): $(TOP)/$(TIRTC_DIR)/ui/lv_conf.h
ifneq ($(TIRTC_ASSET_INSTALLER),y)
$(CUSTOM_COBJS): $(TIRTC_AEC_STAMP)
# The SDK does not load generated .d files; rebuild the adapter when its
# included AEC implementation changes.
$(BUILDDIR)/$(TIRTC_DIR)/media/audio_device.o $(BUILDDIR)/$(TIRTC_DIR)/media/audio_device.pp: \
    $(TOP)/$(TIRTC_DIR)/media/audio_aec.inc
endif

$(BUILDDIR)/$(BINNAME).elf: $(TOP)/$(TIRTC_DIR)/tirtc_app.mk

ifeq ($(TOOLCHAIN),GCC)
libusr-y += lib_tirtc_v04_ui.a
$(BUILDDIR)/lib/libusr/lib_tirtc_v04_ui.a: $(CUSTOM_COBJS)
	@mkdir -p $(dir $@)
	$(ECHO) AR $@
	$(Q)$(AR) -cr $@ $^
endif
