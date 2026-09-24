# pro 交付固件与验证记录

[返回项目首页](../../examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/README.md) · [编译烧录与资源安装](../../examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/docs/当前设备操作.md#build-flash)

首次使用请下载 [pro 最新固件](../../firmware/pro/latest.binpkg)，或在仓库根目录执行 `build_tirtc.bat pro`。该出厂初始化包包含正常应用、配套底包和外置 Flash 字库/背景，**一次全量烧录即可；会覆盖外置 Flash 文件，请先备份需要保留的文件。** 操作见 [烧录与资源准备](../../examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/docs/当前设备操作.md#build-flash)。

本目录保留应用单独构建包、独立资源安装程序及历史包，供开发维护和回退；下表校验值针对这些文件，不是 `firmware/pro/latest.binpkg` 的校验值。

## 最新出厂包验证（2026-09-24）

- 在 GitHub 发布目录移开旧编译输出与已解压工具链，按首页命令从零构建：`build_tirtc.bat lite` 首次自动解压工具链并编译成功，`build_tirtc.bat pro` 编译、打包通过；维护用 `build_tirtc_pro_assets.bat` 也单独从零构建通过。
- 最新出厂包大小为 7,566,228 字节。包内启动程序、系统、通信底包和应用四项载荷与本次 `gccout/tirtc_pro` 生成的正常应用包逐字节一致；新增 SPI1 外置文件系统一项，容量 4 MiB、偏移 0。
- `firmware/pro/latest.binpkg` 来自本次发布目录构建；下方历史命名包保留原文件。构建路径会进入 LittleFS 的断言字符串，不同目录构建的应用哈希可能不同，不能用历史包哈希校验最新包；业务源码、库及资源不因此改变。
- 外置镜像使用 LittleFS 2.0、4 KiB 块，字体和背景逐文件读回校验通过，与正常应用要求的长度、CRC 和 SHA-256 一致。
- 当前校验值见 [SHA256SUMS.txt](../../firmware/pro/SHA256SUMS.txt)，包内项目与资源检查见 [manifest.json](../../firmware/pro/manifest.json)。
- **本次完成了电脑端构建与包内容检查，尚未实机验证整包外闪烧录。** 首次烧录需确认下载工具的内部与 SPI1 外置镜像均成功写入，再验证字体、背景、触摸及音视频功能。

## 应用与维护包：双产品目录迁移及正式命名（2026-09-24）

| 用途 | 下载 | 包大小 | APP Flash / 静态 RAM |
| --- | --- | --- | --- |
| 正常应用 | [TiRTC_pro_NT26F6D0_47.binpkg](TiRTC_pro_NT26F6D0_47.binpkg) | 3,371,560 B | 1,051,296 / 489,332 B |
| UI 资源安装 | [TiRTC_pro_Assets_NT26F6D0_47.binpkg](TiRTC_pro_Assets_NT26F6D0_47.binpkg) | 3,154,352 B | 834,088 / 3,268 B |

SHA-256（下载后核对；完整清单见 [SHA256SUMS.txt](SHA256SUMS.txt)）：

- 正常应用：`eb4350f814e5e6efb91cc81cd73c62f39ef835be1a8863b8e4c3dd579609ab52`。
- 资源安装：`d15ac8e0ba1e9e7a04937a8236cf51d3329f74dd2e6e4adbbdcfae2b0ae4f2c0`。

### 这次做了什么，验证到了哪里

- 将触屏摄像头应用完整迁入正式目录 `tirtc_pro`，根目录 `build_tirtc_pro.bat` 与 `build_tirtc_pro_assets.bat` 分别构建正常应用和资源安装程序。
- 按正式名称全量重编，两套产物均已独立完成编译、链接和完整打包；输出分别为 `gccout/tirtc_pro`、`gccout/tirtc_pro_assets`。18 组构建参数路由检查通过。
- 正常应用的 APP Flash 与静态 RAM 用量均与迁移前 47 版一致；这不是运行时堆峰值，也不代表应用二进制逐字节相同。目录变化会改变编译进程序的源码路径字符串。
- 正常应用 `.bin` SHA-256：`5882b11508ca0060cea1129f6a8d0d923a1f63390300deb7ff3993f4d757a941`。改名前后核对普通应用 316 对、资源安装程序 16 对目标文件：差异限于 littlefs 的 `__FILE__` 路径字符串增加 1 字节及相应断言指针调整，其他执行指令一致；业务源码、配置和库保持原内容。
- 保留 pro 已验证的 1.6.04_beta 兼容底包、TiRTC 2.5.0 库和对应头文件，公共 SDK 源码与工具链由两产品共用；没有改为 lite 的底包或业务配置。
- **本次未烧录，未新增实机音视频、触摸及稳定性验证。** 下载后按 [操作篇](../../examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/docs/当前设备操作.md) 验收；已有正常 UI 资源无需重新安装。

以下仅适用于独立安装器维护模式：先运行资源安装包，看到 `[assets14] INSTALL_OK` 后再烧录正常应用。资源安装包中的应用版本 47 是构建版本，配套字体/背景数据仍为现有资源，不能把资源安装程序长期当作正常应用运行。默认出厂包已包含资源，首次使用无需执行这套两次烧录流程。

[六段操作录像](../../examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/README.md#demo-videos) 未注明对应固件版本，只作功能演示，不能据此把“未做本版实机验证”改成已验证。

## 迁移前应用：AI 呼叫兼容复核（47 版）

`TiRTC_UI_V04_NT26F6D0_47_sdk2.5.0_ai_call_compat.binpkg`

- 以同芯片利尔达 Demo 的联系人刷新、确认、资源释放和通话接管流程为主。
- 修复旧式 call_intent/ai_call_intent 被回传“仅支持 AI 语音对话”、ESP32 字段不兼容和数字请求编号无回包的问题；普通呼叫仍默认视频。
- 仅调整 tirtc 内 AI 模块和版本配置；原 calls、contacts、音视频、UI 及共用 SDK 源码未改。
- 314 项应用回归、11 项联系人刷新检查、56 条原协议断言和 76 条跨 Demo 断言通过；ARM 严格语法检查及整包编译通过。
- 包大小：3,371,560 字节；APP：1,051,296 字节；静态 RAM 范围：489,332 字节。比 46 版增加 440 字节程序空间、128 字节静态 RAM。
- SHA-256：`09bda26482f797b256d65c424867c34329c270febaa43dab87e51c023e133a3a`。
- **尚未烧录、未做本版实机呼叫验证，也未修改云端角色。** 角色及插件仍需允许视频，旧资源无需重装。
- [审核记录](../../examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/ai/AI_CALL_REVIEW.md) · [配置说明](../../examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/ai/AI_CALL.md)

## 前一版：AI 呼叫联系人（46 版）

`TiRTC_UI_V04_NT26F6D0_46_sdk2.5.0_ai_call.binpkg`

- AI 可呼叫微信及设备联系人，普通呼叫默认视频；明确指定音频时发起语音通话。
- 复用原通话页面、媒体处理及挂断流程；本次源码修改仅在 `tirtc` 应用目录，共用 SDK 未修改。
- 需在设备使用的云端角色中启用 `call_contact` 插件，默认媒体设为 `video`，见 [AI_CALL.md](../../examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/ai/AI_CALL.md)。
- 整包编译、ARM 严格语法检查、309 项回归检查、新增 11 项联系人刷新检查和 56 条协议断言通过。
- 包大小：3,371,120 字节；APP：1,050,856 字节；静态 RAM 范围：489,204 字节。相对 45 版分别增加 5,080 字节程序空间、960 字节静态 RAM。
- SHA-256：`1787d257d3ae8e0998d3215918773266d97287075c824c2ce254c5ea09ac2f87`。
- **本次尚未烧录，也未修改平台角色；实机呼叫仍需验证。** 已有外置 UI 资源继续使用，无需重装或格式化。

## 上一版应用：SDK 同步（45 版，保留供回退）

`TiRTC_UI_V04_NT26F6D0_45_sdk2.5.0_sdk_sync_20260923.binpkg`

- 从清理同步后的源码全量编译，回退时使用此文件。
- 功能版本仍为 45，TiRTC 为 2.5.0；SDK 目录同步没有增加、删除或调整业务功能。
- 整理前后的应用 `.bin`、`.elf` 逐字节相同，摄像头颜色/方向、音视频、UI、网络和分区保持原有实现。
- 包大小：3,366,040 字节；APP：1,045,776 字节；静态 RAM 范围：488,244 字节。
- SHA-256：`a3af9552120290469644db50c6c536a3bf43d65e007d376b4bad9b5f6d9065bb`。
- 本次已全量编译正常应用、外置资源安装程序，并编译检查原厂 `BUILD_MODE=app` 分支。
- **尚未烧录设备，需要你进行实机验证。**

## 历史外置 UI 资源安装包（维护用）

`TiRTC_UI_Assets_V04_NT26F6D0_14.binpkg` 为已有配套资源安装程序，继续保留。
该历史维护流程仅适用于独立安装器：先安装资源，出现 `INSTALL_OK` 后再烧录正常应用。首次使用默认下载前面的最新出厂包，不需要这一步。
`assets14` 保存配套资源，`SHA256SUMS.txt` 为文件校验值。

目录差异及构建方式见
[SDK_DIFF.md](../../examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/SDK_DIFF.md)。
