CUSTOM_DIR := examples/$(PROJECT)

# Product code and compatibility dependencies are independent. Only the
# vendor SDK source and toolchain are shared; keep legacy commands on lite.
TIRTC_EXPLICIT_VARIANT := $(strip $(APP_VARIANT))
APP_VARIANT ?= $(if $(filter tirtc,$(BUILD_MODE)),pro,lite)
ifneq ($(APP_VARIANT),lite)
ifneq ($(APP_VARIANT),pro)
$(error Unsupported APP_VARIANT "$(APP_VARIANT)"; use lite or pro)
endif
endif

ifeq ($(APP_VARIANT),pro)
BUILD_MODE ?= tirtc
ifneq ($(BUILD_MODE),tirtc)
$(error pro requires BUILD_MODE=tirtc; use build_tirtc_pro.bat)
endif
TIRTC_DIR := $(CUSTOM_DIR)/tirtc_pro
IOINI := $(TOP)/$(CUSTOM_DIR)/iodriver.ini
# pro reclaims TTS space; its generated layout must be used for packaging.
APP_PACKAGE_MEMMAP = $(BUILDDIR)/mem_map.txt
include $(TOP)/$(TIRTC_DIR)/config
include $(TOP)/$(TIRTC_DIR)/tirtc_app.mk
else
ifneq ($(TIRTC_EXPLICIT_VARIANT),)
ifneq ($(strip $(BUILD_MODE)),)
ifneq ($(BUILD_MODE),demo)
$(error APP_VARIANT=lite requires BUILD_MODE=demo)
endif
endif
ifneq ($(strip $(DEMO_NAME)),)
ifneq ($(DEMO_NAME),tirtc_app)
$(error APP_VARIANT=lite requires DEMO_NAME=tirtc_app; omit APP_VARIANT for official demos)
endif
endif
endif
ifeq ($(BUILD_MODE),tirtc)
$(error lite requires BUILD_MODE=demo DEMO_NAME=tirtc_app; use build_tirtc_lite.bat)
endif
ifeq ($(TIRTC_ASSET_INSTALLER),y)
$(error The external UI resource installer belongs to pro only)
endif

# Select the product application before loading a variant-specific config.
# Command-line BUILD_MODE/DEMO_NAME values still take precedence over these
# defaults, so the restored official demos remain directly buildable.
BUILD_MODE ?= demo
DEMO_NAME ?= tirtc_app

ifeq ($(BUILD_MODE), demo)
ifeq ($(DEMO_NAME), tirtc_app)
include $(TOP)/$(CUSTOM_DIR)/tirtc_lite/config/tirtc_config.mk
else
include $(TOP)/$(CUSTOM_DIR)/config
endif
else
include $(TOP)/$(CUSTOM_DIR)/config
endif

IOINI := $(TOP)/$(CUSTOM_DIR)/iodriver.ini

CFLAGS_INC += -I $(TOP)/$(CUSTOM_DIR)/inc

ifeq ($(BUILD_MODE), demo)

ifeq ($(DEMO_NAME), tirtc_app)

include $(TOP)/$(CUSTOM_DIR)/tirtc_lite/tirtc_app.mk

else ifeq ($(DEMO_NAME), hardware)

CFLAGS_INC += -I $(TOP)/$(CUSTOM_DIR)/demo/inc

CUSTOM_SRC := $(CUSTOM_DIR)/demo/src
CUSTOM_COBJSTEMP := $(CUSTOM_SRC)/0demo_main.o

BUILD_DRIVER_LCD_EN ?= y
BUILD_DRIVER_WS2812B_EN ?= y
BUILD_DRIVER_CAMERA_EN ?= y
BUILD_DRIVER_TP_EN ?= y

ifeq ($(HWDEMO_KEY_EN), y)
CUSTOM_COBJSTEMP += $(CUSTOM_SRC)/demo_key.o
CFLAGS_DEFS += -DHWDEMO_KEY_EN
endif

ifeq ($(HWDEMO_LCD_EN), y)
CUSTOM_COBJSTEMP += $(CUSTOM_SRC)/demo_lcd.o
CFLAGS_DEFS += -DHWDEMO_LCD_EN
endif

ifeq ($(HWDEMO_CAMERA_EN), y)
CUSTOM_COBJSTEMP += $(CUSTOM_SRC)/demo_camera.o
CFLAGS_DEFS += -DHWDEMO_CAMERA_EN
endif

ifeq ($(HWDEMO_LCD_SSD1306_EN), y)
CUSTOM_COBJSTEMP += $(CUSTOM_SRC)/demo_lcd_ssd1306.o
CFLAGS_DEFS += -DHWDEMO_LCD_SSD1306_EN
endif

ifeq ($(HWDEMO_TP_EN), y)
CUSTOM_COBJSTEMP += $(CUSTOM_SRC)/demo_tp.o
CFLAGS_DEFS += -DHWDEMO_TP_EN
endif

ifeq ($(HWDEMO_SOUND_EN), y)
CUSTOM_COBJSTEMP += $(CUSTOM_SRC)/demo_sound.o
CFLAGS_DEFS += -DHWDEMO_SOUND_EN
endif

ifeq ($(HWDEMO_WS2812B_EN), y)
CUSTOM_COBJSTEMP += $(CUSTOM_SRC)/demo_ws2812b.o
CFLAGS_DEFS += -DHWDEMO_WS2812B_EN
endif

else ifeq ($(DEMO_NAME), demo_zhongyi_ai)

CFLAGS_INC += -I $(TOP)/$(CUSTOM_DIR)/demo/inc
CUSTOM_SRC := $(CUSTOM_DIR)/demo/src/demo_zhongyi_ai/src
CUSTOM_INC := $(CUSTOM_DIR)/demo/src/demo_zhongyi_ai/inc
CFLAGS_INC += -I $(TOP)/$(CUSTOM_INC)
CUSTOM_COBJSTEMP := $(patsubst $(TOP)/%.c,%.o,$(wildcard $(TOP)/$(CUSTOM_SRC)/*.c))
PP_FILES += $(wildcard $(TOP)/$(CUSTOM_SRC)/*.c)

else
$(error Unsupported DEMO_NAME "$(DEMO_NAME)"; use "tirtc_app", "hardware" or "demo_zhongyi_ai")
endif

else

CUSTOM_SRC := $(CUSTOM_DIR)/app/src
CUSTOM_COBJSTEMP := $(CUSTOM_SRC)/user_main.o

endif

CUSTOM_COBJS := $(addprefix $(BUILDDIR)/, $(CUSTOM_COBJSTEMP))

ifeq ($(TOOLCHAIN),GCC)
libusr-y += lib_L_CT4IT00_YP00W_01_V04.a

$(BUILDDIR)/lib/libusr/lib_L_CT4IT00_YP00W_01_V04.a: $(CUSTOM_COBJS)
	@mkdir -p $(dir $@)
	$(ECHO) AR $@
	$(Q)$(AR) -cr $@ $^
endif

endif # APP_VARIANT
