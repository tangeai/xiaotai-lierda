include $(TOP)/$(TIRTC_DIR)/board_compat/board_compat.mk

CFLAGS_INC += -I $(TOP)/$(TIRTC_DIR) -I $(TOP)/$(TIRTC_DIR)/port -I $(TOP)/$(TIRTC_DIR)/ui -I $(TOP)/$(TIRTC_DIR)/network
CFLAGS_INC += -I $(TOP)/$(TIRTC_DIR)/resources -I $(TOP)/$(TIRTC_DIR)/storage
CFLAGS_INC += -I $(TOP)/$(TIRTC_DIR)/status
CFLAGS_INC += -I $(TOP)/$(TIRTC_DIR)/preferences
CFLAGS_INC += -I $(TOP)/$(TIRTC_DIR)/keys
CFLAGS_INC += -I $(TOP)/$(TIRTC_DIR)/platform
CFLAGS_INC += -I $(TOP)/$(TIRTC_DIR)/ai -I $(TOP)/$(TIRTC_DIR)/media -I $(TOP)/$(TIRTC_DIR)/resource_store
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
CFLAGS_DEFS += -DTIRTC_EXTERNAL_UI_ASSETS=1
endif
# This exact F6D_A OS wrapper leaves the scheduler suspended after allocation
# failure. Re-audit the adapter whenever the vendor archive changes.
ifeq ($(MODEMPKG),F6D_A)
TIRTC_OS_SHA256 := $(word 1,$(shell sha256sum "$(TIRTC_BOARD_BASEPKG)/lib/libliot_os.a"))
ifneq ($(TIRTC_OS_SHA256),7e5aba697546aa3191997b3cea54225ab90797616e27d1731da85f4b4efb9610)
$(error F6D_A libliot_os.a changed; review the task-create failure adapter)
endif
LDFLAGS += -Wl,--wrap=liot_rtos_task_create
TIRTC_MQTT_SHA256 := $(word 1,$(shell sha256sum "$(TIRTC_BOARD_BASEPKG)/lib/libliot_mqtts.a"))
ifneq ($(TIRTC_MQTT_SHA256),75b0cb498eb0c56461c2cfcfd044d7379c104698c5b4d446dd7d17cd93fb8f9f)
$(error F6D_A libliot_mqtts.a changed; review asynchronous MQTT cleanup compatibility)
endif
CFLAGS_DEFS += -DTIRTC_MQTT_F6D_ABI_VERIFIED=1

# App-only camera DMA completion fence. The SDK's frame-end IRQ and DMA-end
# callback release the same capture semaphore; wait for the latter explicitly.
# The copied callback table ABI also depends on the base ELF hash below.
TIRTC_CSPI_SHA256 := $(word 1,$(shell sha256sum "$(TIRTC_BOARD_BASEPKG)/lib/libliot_cspi.a"))
TIRTC_CAMERA_SHA256 := $(word 1,$(shell sha256sum "$(TIRTC_BOARD_BASEPKG)/lib/libliot_camera.a"))
TIRTC_CAMERA_HEADER_SHA256 := $(word 1,$(shell sha256sum "$(TOP)/components/kernel/lierda_api/liot_camera/liot_camera.h"))
ifneq ($(TIRTC_CSPI_SHA256),395c290d3b6636f52f66369f473c005acf8076918ef95c89c6be542aa4168a0f)
$(error F6D_A libliot_cspi.a changed; review the camera DMA completion fence)
endif
ifneq ($(TIRTC_CAMERA_SHA256),82a24ddd04a478bc9258c284ac6a5044d8d566b8c760761042e8bc9c6a7efe75)
$(error F6D_A libliot_camera.a changed; review the camera DMA completion fence)
endif
ifneq ($(TIRTC_CAMERA_HEADER_SHA256),e173ef09a0539d79bbb5c41537ffd47f9cb0263afb9b9663ca63cde67fd2ee09)
$(error F6D_A liot_camera.h changed; review the camera DMA completion fence ABI)
endif
CFLAGS_DEFS += -DTIRTC_CAMERA_F6D_ABI_VERIFIED=1
ifneq ($(TIRTC_ASSET_INSTALLER),y)
LDFLAGS += -Wl,--wrap=liot_cspi_get_intf
endif

# Video buffer sizes and JPEG DecodeLine ownership are verified against these
# untouched SDK artifacts. A vendor upgrade requires a fresh contract audit.
TIRTC_JPEG_SHA256 := $(word 1,$(shell sha256sum "$(TIRTC_BOARD_BASEPKG)/lib/libmm_jpeg.a"))
TIRTC_VIDEO_SHA256 := $(word 1,$(shell sha256sum "$(TIRTC_BOARD_BASEPKG)/lib/libmm_videoutil.a"))
TIRTC_JPEG_HEADER_SHA256 := $(word 1,$(shell sha256sum "$(TIRTC_BOARD_COMPAT)/ecapi/PLAT/prebuild/PLAT/inc/mm_jpeg_if.h"))
ifneq ($(TIRTC_JPEG_SHA256),989e86e328159aa63dac13da5f5b39695cb732fd480cf86632deaef3d877e0e6)
$(error F6D_A libmm_jpeg.a changed; review JPEG buffers and DecodeLine ownership)
endif
ifneq ($(TIRTC_VIDEO_SHA256),2408925fce4103a63fa673dac134da06e7ba76e701e2d5a34fd10ed7e2aae69f)
$(error F6D_A libmm_videoutil.a changed; review video buffer contract)
endif
ifneq ($(TIRTC_JPEG_HEADER_SHA256),5b9b583c78e0eda11935d2eeafc356ac82ed909598050865cb7cc58aeffed644)
$(error F6D_A mm_jpeg_if.h changed; review JPEG API contract)
endif
# The read-only LFS_stat entry preserves NOENT, unlike liot_stat/NVM wrappers.
# Gate its signature/layout/error contract against the audited base package.
TIRTC_PREFS_BASE_SHA256 := $(word 1,$(shell sha256sum "$(TIRTC_BOARD_BASEPKG)/ap_lierda_app.elf"))
TIRTC_PREFS_API_SHA256 := $(word 1,$(shell sha256sum "$(TIRTC_BOARD_BASEPKG)/lib/lib_baseapi.a"))
TIRTC_PREFS_NV_SHA256 := $(word 1,$(shell sha256sum "$(TIRTC_BOARD_BASEPKG)/lib/libliot_nv.a"))
ifneq ($(TIRTC_PREFS_BASE_SHA256),8bc2f616e021fb1c98f36afcf2a7f09d0de02937e6cf00909c202b920b0301e0)
$(error F6D_A base ELF changed; review preference stat error compatibility)
endif
ifneq ($(TIRTC_PREFS_API_SHA256),7f9129e313f6af63014c574bbf92eb27b583905c19ef1a292782b49e71767308)
$(error F6D_A lib_baseapi.a changed; review preference stat ABI)
endif
ifneq ($(TIRTC_PREFS_NV_SHA256),2b3e1d1d52b0decb4e72a2931634526f59596aa12bbb52c4c01e3bdfadeffafd)
$(error F6D_A libliot_nv.a changed; review preference NVM return/path semantics)
endif
CFLAGS_DEFS += -DTIRTC_PREFS_F6D_ABI_VERIFIED=1
else
$(error This standalone board port is verified only against F6D_A)
endif
TIRTC_CFILES := $(TOP)/$(TIRTC_DIR)/user_main.c \
                $(TOP)/$(TIRTC_DIR)/remote/tirtc_remote.c \
                $(TOP)/$(TIRTC_DIR)/port/tirtc_port.c \
                $(TOP)/$(TIRTC_DIR)/port/tirtc_rtos_compat.c \
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
                $(TOP)/$(TIRTC_DIR)/port/tirtc_rtos_compat.c \
                $(TOP)/$(TIRTC_DIR)/resource_store/installer_main.c \
                $(TOP)/$(TIRTC_DIR)/resource_store/asset_installer.c \
                $(TOP)/$(TIRTC_DIR)/resource_store/asset_io.c \
                $(TOP)/$(TIRTC_DIR)/resource_store/installer_blob.c
endif
CUSTOM_COBJSTEMP := $(patsubst $(TOP)/%.c,%.o,$(TIRTC_CFILES))
CUSTOM_COBJS := $(addprefix $(BUILDDIR)/,$(CUSTOM_COBJSTEMP))
PP_FILES += $(CUSTOM_COBJS:.o=.pp)

ifeq ($(TOOLCHAIN),GCC)
libusr-y += lib_tirtc_v04_ui.a
$(BUILDDIR)/lib/libusr/lib_tirtc_v04_ui.a: $(CUSTOM_COBJS)
	@mkdir -p $(dir $@)
	$(ECHO) AR $@
	$(Q)$(AR) -cr $@ $^
endif

ifneq ($(TIRTC_ASSET_INSTALLER),y)
include $(TIRTC_BOARD_COMPAT)/lvgl.mk
endif
