CUSTOM_DIR := examples/$(PROJECT)

# Select the product application before loading a variant-specific config.
# Command-line BUILD_MODE/DEMO_NAME values still take precedence over these
# defaults, so the restored official demos remain directly buildable.
BUILD_MODE ?= demo
DEMO_NAME ?= tirtc_app

ifeq ($(BUILD_MODE), demo)
ifeq ($(DEMO_NAME), tirtc_app)
include $(TOP)/$(CUSTOM_DIR)/tirtc/config/tirtc_config.mk
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

include $(TOP)/$(CUSTOM_DIR)/tirtc/tirtc_app.mk

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
