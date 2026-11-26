# NT26F9D0/F9D_D, vendor SDK 1.7.0, exact source commit
# 59f1250c4b8caad4bbf4f74167b7b39c79097f8c.
# See boards/f9d_d/ABI.md for the verified link and layout contracts.
# Compare the artifact bytes directly, so command-line make variables cannot
# impersonate a successful checksum calculation or a verification flag.
ifneq ($(word 1,$(shell sha256sum "$(TOP)/rules/Makefile.defs")),6363537325248250fcaff7b9df7f04e73becb201ae60bf95392d61379fa64f83)
$(error F9D_D 1.7.0 SDK version definitions changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(TOP)/rules/Makefile.modem")),66a165427a35d79db12fcbde69ea773a84304a7de78c8470991d15e667d1e8bb)
$(error F9D_D 1.7.0 modem build rules changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(TOP)/components/kernel/core/main.c")),55b14cf0477f8fc69c733d6ba284bbe42d287dcf5fe1f3fdba7e0e692c03a9a3)
$(error F9D_D 1.7.0 application startup changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(BASEPKG_MDM_DIR)/ap_lierda_app.elf")),9a207e256c2ef91eb98dc09e094a729275aa02118d073bd0d0baef1173462815)
$(error F9D_D 1.7.0 F9D_D base ELF changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(BASEPKG_MDM_DIR)/lib/libliot_audio2.a")),be9a0e55e895e05df5a5109940b644c84fd8dd94e78da1b9979a58a2e5f4202b)
$(error F9D_D 1.7.0 libliot_audio2.a ABI changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(BASEPKG_MDM_DIR)/lib/libliot_mqtts.a")),693bb52526fd09c165a4c5fbfa1111c79fa2bec7a8570356f86711aa3dff0237)
$(error F9D_D 1.7.0 libliot_mqtts.a ABI changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(BASEPKG_MDM_DIR)/lib/libliot_nv.a")),8c1358539f7f540d4559f883c0bc15df611e6c7e9e80271369c691bd446ff055)
$(error F9D_D 1.7.0 libliot_nv.a ABI changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(BASEPKG_MDM_DIR)/lib/libliot_datacall.a")),99ce47c23d9a446766948f0a0c5e041d87936af048f68325a95f857dd9cd42b7)
$(error F9D_D 1.7.0 libliot_datacall.a changed; re-audit bearer state error semantics before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(BASEPKG_MDM_DIR)/lib/libliot_camera.a")),3bf927ccb75ef6d10068cd19d47f9bcedbb6220a6ca66f4fef3bff24e3b7a8ea)
$(error F9D_D 1.7.0 libliot_camera.a ABI changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(BASEPKG_MDM_DIR)/lib/libliot_cspi.a")),674092f3588a603d1f9cb05be7f971756c7f53d04e4a332c7ab9908b0b09e528)
$(error F9D_D 1.7.0 libliot_cspi.a ABI changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(BASEPKG_MDM_DIR)/lib/libmm_jpeg.a")),ce6185dfd4e3954fc9babee173f3b3f83df8cebf7ffdec094e99493e9cd497cc)
$(error F9D_D 1.7.0 libmm_jpeg.a ABI changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(BASEPKG_MDM_DIR)/lib/libmm_videoutil.a")),98cb2609ffa2fbea1d4547c8c7914255f7aa79f4705144c0b9f8e3065d617b94)
$(error F9D_D 1.7.0 libmm_videoutil.a ABI changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(BASEPKG_MDM_DIR)/lib/lib_baseapi.a")),5ce01ee546cba3893e77f063eb2fc031678b90d1ed644e3e4f637caced675e2b)
$(error F9D_D 1.7.0 lib_baseapi.a ABI changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(TOP)/components/kernel/lierda_api/liot_camera/liot_camera.h")),e173ef09a0539d79bbb5c41537ffd47f9cb0263afb9b9663ca63cde67fd2ee09)
$(error F9D_D 1.7.0 camera public header changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(TOP)/components/kernel/ecapi/PLAT/prebuild/PLAT/inc/mm_jpeg_if.h")),5b9b583c78e0eda11935d2eeafc356ac82ed909598050865cb7cc58aeffed644)
$(error F9D_D 1.7.0 JPEG public header changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(TOP)/components/kernel/lierda_api/liot_audio2/liot_audio2.h")),09240148a04a6c0d6073dd91352ed2cc89bbaffc1d831f045a6de160398e4b6e)
$(error F9D_D 1.7.0 liot_audio2.h public ABI changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(TOP)/components/kernel/lierda_api/liot_mqtt/liot_mqtt_client.h")),98c1b9a63e796df25be3f7386a98e31f8441d83ef687325e29ce501edac1c3f1)
$(error F9D_D 1.7.0 liot_mqtt_client.h public ABI changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(TOP)/components/kernel/lierda_api/liot_nv/liot_nv.h")),dd32b909c35731d33e7e6ef324e26341a708539561e921039faf0486288511b3)
$(error F9D_D 1.7.0 liot_nv.h public ABI changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(TOP)/components/kernel/lierda_api/liot_datacall/liot_datacall.h")),b97e0cea7062ed38709f8b21c7baa04e08b14027de8c93179c2901fe29fd9a78)
$(error F9D_D 1.7.0 liot_datacall.h public ABI changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(TOP)/components/kernel/lierda_api/liot_fs/liot_fs_api.h")),6609b8dc0b0f077d1d46cb3b2f1b3a605d378028ead8603fbfe880fd607491e3)
$(error F9D_D 1.7.0 liot_fs_api.h public ABI changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(TOP)/components/kernel/lierda_api/liot_os/liot_os.h")),3ccdb10842572ede64e16a661ffb1eb1bda824a93fac076ef4dabb6a988bbf29)
$(error F9D_D 1.7.0 liot_os.h public ABI changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(TOP)/components/kernel/lierda_api/liot_cspi/liot_cspi.h")),aad634f683e4802af09827c5e5946edac1125e8ea6144d3ae333c766ad9f55a2)
$(error F9D_D 1.7.0 liot_cspi.h public ABI changed or missing; re-audit before building)
endif
ifneq ($(word 1,$(shell sha256sum "$(TOP)/components/kernel/lierda_api/liot_i2s/liot_i2s.h")),2cd4750589ad60aeb5899fd020d3a45b5e5ce14b7c955e4d0efdfbbe7086e506)
$(error F9D_D 1.7.0 liot_i2s.h public ABI changed or missing; re-audit before building)
endif
override TIRTC_F9_RUNTIME_BASE_SHA256 := 9a207e256c2ef91eb98dc09e094a729275aa02118d073bd0d0baef1173462815
CFLAGS_DEFS += -DTIRTC_MQTT_BASE_ABI_VERIFIED=1
CFLAGS_DEFS += -DTIRTC_PREFS_BASE_ABI_VERIFIED=1
CFLAGS_DEFS += -DTIRTC_CAMERA_BASE_ABI_VERIFIED=1
ifneq ($(TIRTC_ASSET_INSTALLER),y)
LDFLAGS += -Wl,--wrap=liot_cspi_get_intf
endif
