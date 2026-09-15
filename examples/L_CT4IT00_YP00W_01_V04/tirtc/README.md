<!-- SPDX-FileCopyrightText: 2026 Shenzhen Tange Intelligent Technology Co., Ltd. <https://tange.ai> -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# L_CT4IT00 TiRTC application

This directory contains the product application for the Lierda NT26F6D0
OpenCPU board.  It provides the three-key OLED UI, LTE/network time, device
binding, permanent MQTT, AI chat, WeChat voice calls via the “探鸽小钛” mini program,
device-to-device voice calls, platform LIVE intercom, independent GROUP room
intercom and shared ES8311 audio.

The product code is intentionally separate from Lierda's demonstration tree:

- `src/app`: task composition only;
- `src/core`: one process-wide TiRTC lifecycle, connection owner and verified platform failure adapters;
- `src/services`: LTE, binding and MQTT transports;
- `src/features`: independent AI/WX/DEV/LIVE/GROUP state machines;
- `src/media`: shared audio and codecs;
- `src/ui`: key input, OLED primitives and menu policy;
- `include`: public interfaces between those layers;
- `sdk`: the unmodified, version-locked TiRTC header/archive;
- `board` and `config`: target IO and build-time product policy.

## Documentation

- [当前设备操作与 UI 状态说明](docs/当前设备操作.md)
- [AI 呼叫微信与设备配置图文详解](docs/AI呼叫微信与设备配置详解.md)
- [代码与流程详解](docs/代码与流程详解.md)
- [TiRTC 接口调用与模块流程](docs/TiRTC接口调用与模块流程.md)
- [源码来源记录](SOURCE_PROVENANCE.md)
- [第三方组件与发布边界](THIRD_PARTY_NOTICES.md)

GROUP 操作集中在《当前设备操作》第 10A 章，内部流程在《代码与流程详解》第 17A 章，
SDK 调用在《TiRTC 接口调用与模块流程》第 9A 章；不再单独维护需求草案和过程记录。

两项功能的上手入口：

- **AI 呼叫微信与设备**：先准备联系人，再按[AI 呼叫配置](docs/AI呼叫微信与设备配置详解.md)启用 `call_contact` 插件、保存并绑定到设备；语音转接复用原 WX/DEV 流程。[演示视频](../../../README.md#video-ai-call-contacts)。
- **GROUP 多人对讲**：在[我的设备](https://xiaotai.chat/devices)打开设备的“多人对讲”，由一台设备创建房间，其他设备用房间号加入；本机仍需在空闲主页长按返回约 2 秒进入。网页步骤及试音见[操作说明第 10A 章](docs/当前设备操作.md#10a-独立多人对讲group)，[演示视频](../../../README.md#video-group-intercom)。

Application-source licensing is declared per file. The supplied TiRTC headers
and binary SDK are separate proprietary materials and are not relicensed by
this repository. This release includes the matching SDK required to build the
application. Component licences and redistribution boundaries are documented in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

The historical migration notes and the old 20-second record/playback test are
not part of this product tree.  Hardware demonstrations remain available in
the upstream Lierda examples.

## 默认构建

在 SDK 根目录执行，修改功能或日志开关时换用新的输出目录：

```powershell
.\build.bat build PROJECT=L_CT4IT00_YP00W_01_V04 MODEM=NT26F6D0 MODEMPKG=F6D_A BUILDDIR="$PWD/gccout/tirtc-current"
```

`config/tirtc_config.mk` defaults to `DEMO_NAME=tirtc_app`. Build-time log
policy is kept there (currently SDK 3, transport 3, statistics disabled). Restricted-network mode is intentionally fixed in the
runtime adapter for this cellular target; see
[代码与流程详解](docs/代码与流程详解.md).

`HWDEMO_GROUP_ROOM_EN=y` enables the independent room feature by default.
Hold BACK at the ready home screen for about two seconds to enter the assigned
room when AI/WX/DEV are idle; CONFIRM toggles the microphone while reception continues, and BACK exits
the local session. Room assignment stays on the server. The hold threshold is
`TIRTC_GROUP_BACK_HOLD_MS=2000`; use a separate build output directory when
changing feature flags. See [当前设备操作](docs/当前设备操作.md), section 10A.

External JSON parsing now rejects nesting deeper than 16 levels before entering
cJSON. F6D_A task creation also has an application-side failure adapter, guarded
by the supplied OS archive hash. Successful calls and business state machines
keep their existing behavior; supplied SDK archives are unchanged. See
[代码与流程详解](docs/代码与流程详解.md), sections 4.2 and 21.4.

## 当前固件与验证

上述命令生成 `gccout/tirtc-current/L_CT4IT00_YP00W_01_V04_NT26F6D0_01.binpkg`，
已包含底包和应用，沿用 NT26F6D0 / F6D_A 的完整烧录配置。仓库
[当前完整烧录包](../../../firmware/L_CT4IT00_YP00W_01_V04_NT26F6D0_01.binpkg)
已于 2026-09-15 从开发仓库源码提交 `8bee220bf88280e1a344c5006ab0245b4556a115` 编译更新，
本仓库同步了相同的应用源码；该提交号属于开发仓库，用于固件溯源。
包含 AI 呼叫联系人、GROUP 多人对讲及应用异常防护。历史 `0.1.0` 固件不包含 GROUP 与这些异常防护；
`gccout/` 是本地构建输出，不随仓库发布。

本次发布包校验信息：

- 大小：3079056 B（约 2.94 MiB）；SHA-256：`c27beac18996674fdbc49959892de27e2806b2d551cbedfb3954e7da1d092d42`。
- Flash：758792 B；静态 RAM：154212 B。以上不代表运行时堆峰值或任务栈余量。
- 该包已编译、链接、打包通过；当前代码保留原 1 包 AI 起播和原主页长按逻辑。

GROUP、UI、WX/DEV 交接、MQTT、runtime、JSON 深度保护及任务创建失败分支的主机检查
已通过，不能代替实机声音和长期稳定性验证。试烧需用至少三台设备检查同时发言混音、
持续收听与切麦、进出房间；微信/设备来电接听、拒接或立即取消后，应恢复关麦收听。
退出 GROUP 后再检查 AI/WX/DEV/LIVE 切换、断网恢复和堆/栈余量。
主页长按的现有限制见[操作说明第 10A 章](docs/当前设备操作.md#10a-独立多人对讲group)，
其他已知边界见[代码与流程详解第 25 章](docs/代码与流程详解.md#25-当前已知限制)。
