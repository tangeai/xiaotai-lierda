
LVGL_DIR := $(TOP)/components/thirdparty/lvgl/lvgl-8.3.10

CFLAGS_INC += -I $(LVGL_DIR) \
				-I $(LVGL_DIR)/demos

LVGL_SRC_DIRS += $(LVGL_DIR)	\
				$(LVGL_DIR)/demos			\
				$(LVGL_DIR)/demos/*			\
				$(LVGL_DIR)/demos/*/*		\
				$(LVGL_DIR)/src		\
				$(LVGL_DIR)/src/*	\
				$(LVGL_DIR)/src/*/*		\
				$(LVGL_DIR)/src/*/*/*	

CFLAGS += -DFEATURE_LVGL_ENABLED
CFLAGS += -DLV_LVGL_H_INCLUDE_SIMPLE

LVGL_EXCLUDE_FILES :=
LVGL_CSRC = $(foreach dir, $(LVGL_SRC_DIRS), $(wildcard $(dir)/*.c))
LVGL_CFILES = $(filter-out $(LVGL_EXCLUDE_FILES), $(LVGL_CSRC))
LVGL_COBJSTEMP := $(patsubst $(TOP)/%.c, %.o, $(LVGL_CFILES))
LVGL_COBJS := $(addprefix $(BUILDDIR)/, $(LVGL_COBJSTEMP))

PP_FILES += $(LVGL_COBJS:.o=.pp)

ifeq ($(TOOLCHAIN),GCC)
libusr-y += liblvgl.a

# Native Windows tools do not translate MSYS paths inside response files.
LVGL_AR_INPUTS = $^
ifeq ($(BUILD_ENV),windows)
LVGL_AR_BUILDDIR := $(shell cd "$(BUILDDIR)" && pwd -W)
LVGL_AR_INPUTS = $(patsubst $(BUILDDIR)/%,$(LVGL_AR_BUILDDIR)/%,$^)
endif

$(BUILDDIR)/lib/libusr/liblvgl.a: $(LVGL_COBJS)
	@mkdir -p $(dir $@)
	$(ECHO) AR $@
	$(Q)echo $(LVGL_AR_INPUTS) > $@.rsp
	$(Q)$(AR) $(ARFLAGS) $@ @$@.rsp
	$(Q)rm -f $@.rsp
endif
