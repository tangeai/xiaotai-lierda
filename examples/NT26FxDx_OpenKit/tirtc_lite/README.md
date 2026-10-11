# lite · TiRTC × 利尔达 OpenCPU 语音终端

[原厂 SDK 说明](../../../README.md)

探鸽智能基于利尔达 OpenCPU SDK 开发的 4G 语音设备开源工程，通过 **TiRTC** 实现 AI 对话、通过 AI 呼叫微信和设备联系人、微信对讲、设备互呼、远端语音对讲和独立多人房间对讲。

## 1. 先看设备能做什么

| 功能 | 使用体验 | 操作视频 |
| --- | --- | --- |
| 联网与绑定 | 4G 联网后，点击[绑定设备](https://xiaotai.chat/devices)，登录后输入 OLED 六位绑定码，绑定成功后设备进入主页。 | [设备绑定](#video-binding) |
| AI 语音对话 | 支持 AEC 回声消除和边播边听；AI 回复时可直接插话，由云端判断语音与打断。 | [AI 对讲](#video-ai-talk) |
| AI 呼叫微信与设备 | 配好联系人和 AI 呼叫插件后，说“用微信呼叫妈妈”或“呼叫设备小钛”，自动转入 WX/DEV 呼叫流程。 | [AI 呼叫演示](#video-ai-call-contacts) |
| WX 微信对讲 | 与微信小程序“探鸽小钛”双向呼叫、接听和语音对讲。 | [微信互叫](#video-wechat-call) |
| DEV 设备互呼 | 从联系人列表选择已联网、绑定的设备，呼叫、接听、对讲和挂断。 | [设备互叫](#video-device-call) |
| SET 音频设置 | 调整扬声器音量和麦克风增益，重启后恢复默认。 | [音量设置](#video-audio-settings) |
| LIVE 远端对讲 | 网页远端接入；仅设备在主页且空闲时允许，离开主页后结束。 | [操作说明（第 10 章）](docs/当前设备操作.md) |
| GROUP 多人对讲 | 在[我的设备](https://xiaotai.chat/devices)打开对应设备的“多人对讲”，创建或加入同一房间；设备主页空闲时长按返回约 2 秒进入，确定键切麦，返回退出并保留房间分配。 | [创建房间与对讲](#video-group-intercom) |

设备通过三个按键操作，OLED 显示菜单、网络状态和时间。当前仅传输音频，同一时间运行一个媒体会话。GROUP 中收到微信或设备来电时，先暂停多人对讲；来电流程结束后恢复关麦收听，主页四项菜单不变。

本地构建在根目录 `firmware/lite/YYYY_MM_DD_HH_MM_SS_lite.binpkg` 生成 Lite 完整烧录包，内部包标识为 **lite_release**，包含 AI、AI 呼叫联系人、微信、设备互呼、远端对讲和 GROUP 多人对讲功能。仓库仅保留当前发布包及同目录的 `SHA256SUMS.txt`，历史包与编译中间产物不随仓库发布。文件名时间取对应 `.bin` 的编译完成时间（UTC+8），不是重新命名的时间；克隆后可使用当前发布包，或按第 5 节重新构建。

### 平台设备信息

当前源码在设备上线约 30 秒后、没有通话或多人对讲时，向平台补报以下信息；已有绑定无需解绑，新固件上线后会刷新。配置集中在 [device_metadata.h](include/device_metadata.h)。

| 字段 | Lite 上报值 |
| --- | --- |
| 芯片型号 | `NT26F6D0`（模组型号，填入平台芯片型号栏） |
| 开发板型号 | `L-CT4IT00-YP00W-01-V04` |
| 应用版本 | `0.1.0` |
| 固件版本 | `F6D_D_lite_0.1.0` |
| LIVE / DEV / 微信音频 | G.711 A-law，网络首选 8 kHz、单声道；无视频 |
| LIVE 音频 Stream ID | 上行 `10`，下行 `14`（接收继续兼容 `10`） |

设备互呼实际双向使用 Stream ID `10`，微信使用 `0`；这两类 ID 不在当前上报接口的可填字段中。硬件内部仍使用 16 kHz PCM，不因信息上报改变音频格式。仅上报编译启用的 LIVE、DEV、微信场景；三项全关闭时跳过此可选接口。

上报失败不会取消绑定或阻止媒体启动，最多尝试三次，失败后分别等待 60 / 120 秒并继续等待空闲；重新建立正式连接后再同步。新呼叫若恰好在上报开始后到达，信令请求可能短暂等待原有 HTTP 通道释放。`lite_release` 保留为内部应用构建标识，上述固件版本是平台显示的信息；需烧录包含这些字段的固件后验证，修改源码不会更新已经运行的设备。

首次体验这两项功能：

- **AI 呼叫**：先确认 `WX`/`DEV` 手动呼叫可用，再给设备智能体启用 `call_contact` 插件、保存并绑定；进入 `AI` 说出联系人完整名称。逐项填写和示例图见[AI 呼叫配置](docs/AI呼叫微信与设备配置详解.md)。
- **多人对讲**：网页上由设备 A 创建房间，设备 B 用同一六位房间号加入；两台设备分别长按返回进入 `GROUP`，等待 `LISTENING` 后用确定键开麦试音。网页逐步操作和退出方法见[多人对讲操作](docs/当前设备操作.md#10a-独立多人对讲group)。此功能不需要 AI 插件或 WX/DEV 联系人关系。

### 操作视频

点击标题展开观看或下载。若当前仓库页面不显示播放器，点击“下载原视频”即可；全部原视频随本仓库保存。

<a id="video-binding"></a>
<details open>
<summary>设备绑定</summary>

https://github.com/user-attachments/assets/9f965419-72f7-4b59-abf2-69d00e6f0715

[下载原视频](docs/assets/videos/binding.mp4)

</details>

<a id="video-ai-talk"></a>
<details>
<summary>AI 对讲</summary>

https://github.com/user-attachments/assets/92ab937b-5532-4236-b849-e90380c01f11

[下载原视频](docs/assets/videos/ai-talk.mp4)

</details>

<a id="video-wechat-call"></a>
<details>
<summary>微信互叫</summary>

https://github.com/user-attachments/assets/9f6a4002-9c00-4c24-a1c4-85dbf84ddf69

[下载原视频](docs/assets/videos/wechat-call.mp4)

</details>

<a id="video-device-call"></a>
<details>
<summary>设备互叫</summary>

https://github.com/user-attachments/assets/c138f4ee-6627-421b-b2b0-6c3b4eca0962

[下载原视频](docs/assets/videos/device-call.mp4)

</details>

<a id="video-audio-settings"></a>
<details>
<summary>音量设置</summary>

https://github.com/user-attachments/assets/2fb17e1b-e6a1-487a-8240-e44c93f56bab

[下载原视频](docs/assets/videos/audio-settings.mp4)

</details>

<a id="video-ai-call-contacts"></a>
<details>
<summary>通过 AI 对话呼叫微信和其他设备</summary>

![通过 AI 对话呼叫微信和其他设备](docs/assets/videos/ai-call-contacts.mp4)

[下载原视频](docs/assets/videos/ai-call-contacts.mp4) · [配置与操作步骤](docs/AI呼叫微信与设备配置详解.md)

</details>

<a id="video-group-intercom"></a>
<details>
<summary>创建房间多人对讲</summary>

![创建房间多人对讲](docs/assets/videos/group-intercom.mp4)

[下载原视频](docs/assets/videos/group-intercom.mp4) · [网页建房、加入与按键操作](docs/当前设备操作.md#10a-独立多人对讲group)

</details>

## 2. 购买与准备硬件

购买链接：[利尔达 NT26F6D0 开发套件（淘宝）](https://item.taobao.com/item.htm?ft=t&id=1055493780611&skuId=6093790444060)。

本工程适配 **NT26F6D0 / F6D_D**，搭配 **SSD1306 OLED、三按键、ES8311 音频电路、麦克风和扬声器**。准备可上网的 4G SIM 卡、天线及 USB 数据线；使用其他硬件时需核对引脚与音频配置。

## 3. 跟随利尔达教程上手

先阅读[利尔达快速上手教程](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/general/quick_run_demo.html)，了解硬件连接、驱动安装、编译和烧录流程。

官方教程以 LED 示例入门；体验本项目时使用本页的 **TiRTC 固件与工程参数**，无需修改 LED 示例开关。

| 官方资源 | 链接 |
| --- | --- |
| SDK 开源项目与编译说明 | [SDK 源码](https://github.com/lierda-iot/CAT1.bis_OpenCPU) · [SDK 介绍与开发说明](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/general/quick_start.html) |
| 烧录工具 | [下载工具 ZIP](https://opendocs.lierda.com/fs/lierda_upgrade_tool/lierda_upgrade_tool_latest.zip) · [软件使用与烧录教程](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/tools/flash/flash-tool.html) |
| USB 驱动 | [驱动下载与安装教程](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/tools/usb/USB_Driver_Installation_Guide.html) |

## 4. 烧录并体验 TiRTC

1. **取得固件**：使用根目录 `firmware/lite/` 中的当前时间戳 `.binpkg`，或按第 5 节从源码构建。包包含底包、应用及多人对讲功能，只需这一个 `.binpkg`，适用于上述硬件配置。烧录前核对同目录的 `SHA256SUMS.txt`。
2. **烧录设备**：在利尔达烧录工具中选择“自定义镜像”，载入该文件进行**全量烧录**，具体操作见第 3 节的烧录教程。该文件不是 OTA 升级包。
3. **联网绑定**：接好 SIM 卡与天线后上电，等待 OLED 显示六位绑定码，在[设备演示平台](https://xiaotai.chat/)登录并添加设备，绑定成功后设备进入主页。
4. **开始对讲**：按 `KEY0` 选择、`KEY1` 确认、`KEY2` 返回。使用 AI 前先配置并绑定平台智能体；使用 WX 时，在微信搜索“探鸽小钛”，登录同一账号并完成设备的 VoIP 授权与上报。详细步骤见[当前设备操作](docs/当前设备操作.md)。

**首次 A→D 迁移必须先备份并按流程重新绑定。** Lite 的内部文件系统从旧 A 的 `0x321000 / 0xC3000`（起始地址 / 长度）变为 D 的 `0x371000 / 0x73000`；新 APP 会覆盖旧文件系统前段，不能依赖旧文件保留。烧录期间不要断电。

## 5. 从源码接入与二次开发

获取包含本产品目录和公共 SDK 的完整工程；仅克隆原厂 SDK 不包含这两套产品应用。首次编译会自动解压自带工具链。本仓库已包含 lite 应用及其编译所需的官方 SDK 1.7.0、F6D_D 底包和工具。lite 的业务、配置及 TiRTC 库位于独立的 `tirtc_lite` 目录，不引用 pro 的业务代码。在仓库根目录执行 Windows 编译命令：

```powershell
.\examples\NT26FxDx_OpenKit\tirtc_lite\build.bat
```

编译成功后自动更新根目录 `firmware/lite/YYYY_MM_DD_HH_MM_SS_lite.binpkg` 和同目录的 `SHA256SUMS.txt`，烧录方法同上。`gccout/tirtc_lite` 保存编译中间产物，不随仓库发布。修改功能或日志开关后，应清理该产品的旧输出或使用新的构建输出目录，说明见[应用构建说明](docs/应用说明.md#默认构建)。

<a id="aec-debug"></a>

当前使用官方 SDK 1.7.0 和 `F6D_D` 底包。Lite 与 Pro 共用公共 SDK，各自保留独立应用与构建配置；从旧 A 底包升级时须完整烧录配套底包与 APP。

Lite 默认开启 `TIRTC_AUDIO_AEC_EN=y`，通过利尔达 3A 接口启用 AEC，ANS/AGC 保持旁路。AI 播放期间持续收音上传，配合云端语音检测支持插话打断；微信、设备互呼、LIVE 和 GROUP 使用同一 AEC 音频适配，保留原有接听、开关麦和按键操作。AI 语音结束时保留告别语播放收尾。

AEC 配置位于 `config/tirtc_config.mk`，默认 `TIRTC_AUDIO_AEC_DELAY=10`。详细音频调试日志默认关闭（`src/media/audio_device.c` 中的 `DEMO_AI_AUDIO_DEBUG=0`），保留初始化和错误日志。调试时可执行 `examples\NT26FxDx_OpenKit\tirtc_lite\build_app.bat TIRTC_AUDIO_AEC_EN=n` 关闭 AEC，恢复普通录放音和 AI 半双工；该入口仅更新 `gccout/tirtc_lite` 中的包。

本脚本选择 `APP_VARIANT=lite BUILD_MODE=demo DEMO_NAME=tirtc_app`，并使用独立输出目录；必须明确选择 `APP_VARIANT=lite`；不传 `APP_VARIANT` 时走原厂 app/demo 分支，不会自动选择 Lite。

若使用利尔达工具从源码编译，选择以下参数（路径相对于仓库根目录）：

| 配置项 | 选择 |
| --- | --- |
| 应用文件夹 | `examples/NT26FxDx_OpenKit`（不是下一级 `tirtc_lite` 目录） |
| 底包路径 | `components/basePkg/F6D_D` |
| 模组型号 | `NT26F6D0` |
| 附加编译参数 | `APP_VARIANT=lite BUILD_MODE=demo DEMO_NAME=tirtc_app` |

若工具不能追加上述产品参数，请使用本节的 `tirtc_lite/build.bat`；只选择应用文件夹、底包和模组不会自动编译 Lite。

lite 应用位于 [examples/NT26FxDx_OpenKit/tirtc_lite](.)：利尔达 SDK 提供 4G、系统和硬件能力，应用完成绑定与呼叫信令，TiRTC 负责实时连接及音频、命令传输。

- [当前设备操作](docs/当前设备操作.md)：绑定、按键、AI 呼叫、网页创建/加入多人房间及 OLED 状态。
- [AI 呼叫微信与设备配置](docs/AI呼叫微信与设备配置详解.md)：联系人准备、智能体和设备插件填写示例。
- [代码与流程详解](docs/代码与流程详解.md)：文件职责、模块架构、AI 转呼和多人对讲业务流程。
- [TiRTC 接口调用与模块流程](docs/TiRTC接口调用与模块流程.md)：SDK 调用位置、参数与回调，以及 AI 呼叫命令和多人房间信令。

[TiRTC 官方文档](https://docs.tange.ai/products/tirtc/) · [WebRTC / TiRTC 基础知识](https://github.com/tangeai/xiaotai-esp32/blob/main/docs/README_CN.md)

应用源码、利尔达 SDK、TiRTC SDK 及第三方组件分别遵循各自授权声明。

## 许可

探鸽自有应用代码与文档采用 [MIT](LICENSE)；利尔达派生代码、供应 SDK 和第三方组件保留各自授权。具体范围见 [源码来源](SOURCE_PROVENANCE.md) 与 [第三方说明](THIRD_PARTY_NOTICES.md)。
