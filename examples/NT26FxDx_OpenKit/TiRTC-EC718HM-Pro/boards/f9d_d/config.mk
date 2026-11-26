# NT26F9D0: Lite wiring with the already working Pro screen/touch assembly.
# Exact vendor SDK/ABI identity is checked in abi.mk before any compilation.
TIRTC_CALL_MEDIA_DIAGNOSTICS ?= n
ifeq ($(TIRTC_CALL_MEDIA_DIAGNOSTICS),y)
APP_VERSION := pro_f9d_d_wxav605_sdk170_diag
CFLAGS_DEFS += -DTIRTC_CALL_MEDIA_DIAGNOSTICS=1
else
APP_VERSION := pro_f9d_d_wxav605_sdk170
endif
override BASEPKG_MDM_DIR := $(TOP)/components/basePkg/F9D_D
TIRTC_BOARD_PROFILE_DIR := $(TOP)/$(TIRTC_DIR)/boards/f9d_d
TIRTC_BOARD_LINKER_SCRIPT := $(TIRTC_BOARD_PROFILE_DIR)/app.ld
IOINI := $(TIRTC_BOARD_PROFILE_DIR)/iodriver.ini
CFLAGS_DEFS += -DTIRTC_BOARD_NT26F9D0=1 -DTIRTC_LOCAL_ACK_ENABLE=1
CFLAGS_DEFS += -DTIRTC_DEV_OPUS_ENABLE=1 -DTIRTC_VIDEO_GUI_BACKPRESSURE_ENABLE=1
CFLAGS_DEFS += -DTIRTC_VIDEO_COMPACT_LAYOUT_ENABLE=1 -DTIRTC_VIDEO_RX_PRIORITY=28 -DTIRTC_VIDEO_DIRECT_VIEW_ENABLE=1 -DTIRTC_VIDEO_ACTIVE_GUI_PRIORITY_ENABLE=1
ifeq ($(TIRTC_VIDEO_PERF_DIAGNOSTICS),y)
CFLAGS_DEFS += -DTIRTC_VIDEO_PERF_DIAGNOSTICS=1
endif
# F9 has enough application Flash for the complete UI resource set. Its
# native FS is retained; do not carry the F6D_D 300 KiB FS borrowing here.
ifneq ($(filter-out 0 0x0,$(strip $(CODE_REGION_ADJUST))),)
$(error NT26F9D0 profile requires the unmodified F9D_D partition layout)
endif
CODE_REGION_ADJUST :=
TIRTC_UI_ASSET_MODE := embedded
ifneq ($(TIRTC_UI_ASSET_MODE),embedded)
$(error NT26F9D0 profile requires TIRTC_UI_ASSET_MODE=embedded)
endif
ifeq ($(TIRTC_ASSET_INSTALLER),y)
$(error NT26F9D0 embeds its UI resources; an external-Flash installer is not applicable)
endif

# Connected A/V tasks stay below the vendor CCIO receive task (24).
CFLAGS_DEFS += -DTIRTC_F9_AV_FOCUS=1 -DTIRTC_F9_AV_VIDEO_PRIORITY=22 -DTIRTC_F9_AV_AUDIO_PRIORITY=23
CFLAGS_DEFS += -DTIRTC_F9_WX_SQUARE_VIDEO=1 -DTIRTC_F9_WX_NATIVE_CAPTURE=1

# The verified F9 VPU retry yields only inside the active codec worker.
ifneq ($(word 1,$(shell sha256sum "$(BASEPKG_MDM_DIR)/lib/libdriver_private.a")),68c702aafd007ed26ea4f840e23e1dad2387f1e13fab65382c883aefdeef6f3a)
$(error F9D_D SIPC/VPU archive changed)
endif
CFLAGS_DEFS += -DTIRTC_VIDEO_VPU_YIELD_ENABLE=1
LDFLAGS += -Wl,--wrap=Video_VpuLock -Wl,--wrap=SIPC_ResLock

CFLAGS_DEFS += -DTIRTC_F9_VIDEO_RX_TWO_PENDING_ENABLE=1

# The WX AEC callback ring is scoped to call capture on this board.
CFLAGS_DEFS += -DTIRTC_F9_WX_CAPTURE_RING=1 -DTIRTC_F9_CALL_AUDIO_QOS=1 -DTIRTC_F9_G711_FAST=1 -DTIRTC_F9_WX_ADAPTIVE_PLAYOUT=1 -DTIRTC_F9_WX_PLAY_RETRY_CACHE=1

# Explicit 6/32 KiB socket experiments both log their effective receive quota.
# The exact TiRTC library and vendor AP ABI are already pinned in runtime.mk.
TIRTC_F9_CALL_UDP_RCVBUF_BYTES ?= 0
ifneq ($(TIRTC_F9_CALL_UDP_RCVBUF_BYTES),0)
ifeq ($(filter 6144 32768,$(TIRTC_F9_CALL_UDP_RCVBUF_BYTES)),)
$(error F9 media UDP socket experiment expects 0, 6144 or 32768 bytes)
endif
APP_VERSION := $(APP_VERSION)_r$(TIRTC_F9_CALL_UDP_RCVBUF_BYTES)
CFLAGS_DEFS += -DTIRTC_F9_CALL_UDP_RCVBUF_BYTES=$(TIRTC_F9_CALL_UDP_RCVBUF_BYTES)
LDFLAGS += -Wl,--wrap=socket_udp_bind_addr
endif
