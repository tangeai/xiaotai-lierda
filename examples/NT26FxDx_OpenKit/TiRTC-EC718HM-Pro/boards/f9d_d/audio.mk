# The target pool layout/STOP_CNF ISR contract was independently checked in
# the exact F9D_D 1.7.0 ELF. These Thumb entry points and the buffer pool
# address belong to this F9D_D image.
# The complete ELF and audio archive are checked in abi.mk.
ifneq ($(TIRTC_AUDIO_AEC_EN),y)
$(error F9D_D 1.7.0 currently supports only the audited 3A audio backend)
endif
TIRTC_AUDIO_FREE_GUARD_EN ?= y
ifneq ($(TIRTC_AUDIO_FREE_GUARD_EN),y)
$(error F9D_D 1.7.0 requires the audited late-buffer audio free guard)
endif
CFLAGS_DEFS += -DDEMO_AI_AUDIO_AEC_ENABLE=1 -DDEMO_AI_AUDIO_AEC_DELAY=$(TIRTC_AUDIO_AEC_DELAY)
CFLAGS_DEFS += -DFEATURE_LIERDA_AUDIO_3A -DFEATURE_SUPPORT_APP_PCM_MEM_POOL
CFLAGS_DEFS += -DTIRTC_AUDIO_FREE_GUARD_ENABLE=1
LDFLAGS += -Wl,--wrap=audioFreeRecordBuf -Wl,--wrap=halSetVoiceEngRetCallback
LDFLAGS += -Wl,--defsym=ccioGetAudioServEnt=0x00942fb9
LDFLAGS += -Wl,--defsym=ccioRbufGetFreePcbCnt=0x00937965
LDFLAGS += -Wl,--defsym=ccioRbufUngetPtUlPcb=0x009377e1
LDFLAGS += -Wl,--defsym=halSetVoiceEngRetCallback=0x00a35b55
LDFLAGS += -Wl,--defsym=pro_audio_pcb_pool=0x08060080
