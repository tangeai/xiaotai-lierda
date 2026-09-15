# TiRTC × 利尔达 OpenCPU

探鸽智能基于利尔达 OpenCPU SDK 开发的 4G 语音设备开源工程，通过 **TiRTC** 实现 AI 对话、通过 AI 呼叫微信和设备联系人、微信对讲、设备互呼、远端语音对讲和独立多人房间对讲。

## 1. 先看设备能做什么

| 功能 | 使用体验 | 操作视频 |
| --- | --- | --- |
| 联网与绑定 | 4G 联网后，点击[绑定设备](https://xiaotai.chat/devices)，登录后输入 OLED 六位绑定码，绑定成功后设备进入主页。 | [设备绑定](#video-binding) |
| AI 语音对话 | 向 AI 提问并听取语音回复，设备与 AI 轮流说话（半双工）。 | [AI 对讲](#video-ai-talk) |
| AI 呼叫微信与设备 | 配好联系人和 AI 呼叫插件后，说“用微信呼叫妈妈”或“呼叫设备小钛”，自动转入 WX/DEV 呼叫流程。 | [AI 呼叫演示](#video-ai-call-contacts) |
| WX 微信对讲 | 与微信小程序“探鸽小钛”双向呼叫、接听和语音对讲。 | [微信互叫](#video-wechat-call) |
| DEV 设备互呼 | 从联系人列表选择已联网、绑定的设备，呼叫、接听、对讲和挂断。 | [设备互叫](#video-device-call) |
| SET 音频设置 | 调整扬声器音量和麦克风增益，重启后恢复默认。 | [音量设置](#video-audio-settings) |
| LIVE 远端对讲 | 网页远端接入；仅设备在主页且空闲时允许，离开主页后结束。 | [操作说明（第 10 章）](examples/L_CT4IT00_YP00W_01_V04/tirtc/docs/当前设备操作.md) |
| GROUP 多人对讲 | 在[我的设备](https://xiaotai.chat/devices)打开对应设备的“多人对讲”，创建或加入同一房间；设备主页空闲时长按返回约 2 秒进入，确定键切麦，返回退出并保留房间分配。 | [创建房间与对讲](#video-group-intercom) |

设备通过三个按键操作，OLED 显示菜单、网络状态和时间。当前仅传输音频，同一时间运行一个媒体会话。GROUP 中收到微信或设备来电时，先暂停多人对讲；来电流程结束后恢复关麦收听，主页四项菜单不变。

仓库 `firmware/` 已更新为当前源码编译的完整烧录包，包含 AI 呼叫联系人、GROUP 多人对讲及应用异常防护，可按第 4 节直接下载烧录。历史 `0.1.0` 固件不包含 GROUP 与本次异常防护更新；固件来源和验证说明见[应用说明](examples/L_CT4IT00_YP00W_01_V04/tirtc/README.md#当前固件与验证)。

首次体验这两项功能：

- **AI 呼叫**：先确认 `WX`/`DEV` 手动呼叫可用，再给设备智能体启用 `call_contact` 插件、保存并绑定；进入 `AI` 说出联系人完整名称。逐项填写和示例图见[AI 呼叫配置](examples/L_CT4IT00_YP00W_01_V04/tirtc/docs/AI呼叫微信与设备配置详解.md)。
- **多人对讲**：网页上由设备 A 创建房间，设备 B 用同一六位房间号加入；两台设备分别长按返回进入 `GROUP`，等待 `LISTENING` 后用确定键开麦试音。网页逐步操作和退出方法见[多人对讲操作](examples/L_CT4IT00_YP00W_01_V04/tirtc/docs/当前设备操作.md#10a-独立多人对讲group)。此功能不需要 AI 插件或 WX/DEV 联系人关系。

### 操作视频

点击标题展开视频，可在 GitHub 页面直接播放；全部原视频随本仓库保存，也可通过下方链接下载。

<a id="video-binding"></a>
<details open>
<summary>设备绑定</summary>

https://github.com/user-attachments/assets/9f965419-72f7-4b59-abf2-69d00e6f0715

[下载原视频](assets/videos/binding.mp4)

</details>

<a id="video-ai-talk"></a>
<details>
<summary>AI 对讲</summary>

https://github.com/user-attachments/assets/92ab937b-5532-4236-b849-e90380c01f11

[下载原视频](assets/videos/ai-talk.mp4)

</details>

<a id="video-wechat-call"></a>
<details>
<summary>微信互叫</summary>

https://github.com/user-attachments/assets/9f6a4002-9c00-4c24-a1c4-85dbf84ddf69

[下载原视频](assets/videos/wechat-call.mp4)

</details>

<a id="video-device-call"></a>
<details>
<summary>设备互叫</summary>

https://github.com/user-attachments/assets/c138f4ee-6627-421b-b2b0-6c3b4eca0962

[下载原视频](assets/videos/device-call.mp4)

</details>

<a id="video-audio-settings"></a>
<details>
<summary>音量设置</summary>

https://github.com/user-attachments/assets/2fb17e1b-e6a1-487a-8240-e44c93f56bab

[下载原视频](assets/videos/audio-settings.mp4)

</details>

<a id="video-ai-call-contacts"></a>
<details>
<summary>通过 AI 对话呼叫微信和其他设备</summary>

https://github.com/user-attachments/assets/7851c373-bcab-4646-aa40-3204b7e59e44

[下载原视频](assets/videos/ai-call-contacts.mp4) · [配置与操作步骤](examples/L_CT4IT00_YP00W_01_V04/tirtc/docs/AI呼叫微信与设备配置详解.md)

</details>

<a id="video-group-intercom"></a>
<details>
<summary>创建房间多人对讲</summary>

https://github.com/user-attachments/assets/40d16fd3-29e7-40d9-be6c-e0e52d503b7b

[下载原视频](assets/videos/group-intercom.mp4) · [网页建房、加入与按键操作](examples/L_CT4IT00_YP00W_01_V04/tirtc/docs/当前设备操作.md#10a-独立多人对讲group)

</details>

## 2. 购买与准备硬件

购买链接：[利尔达 NT26F6D0 开发套件（淘宝）](https://item.taobao.com/item.htm?ft=t&id=1055493780611&skuId=6093790444060)。

本工程适配 **NT26F6D0 / F6D_A**，搭配 **SSD1306 OLED、三按键、ES8311 音频电路、麦克风和扬声器**。准备可上网的 4G SIM 卡、天线及 USB 数据线；使用其他硬件时需核对引脚与音频配置。

## 3. 跟随利尔达教程上手

先阅读[利尔达快速上手教程](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/general/quick_run_demo.html)，了解硬件连接、驱动安装、编译和烧录流程。

官方教程以 LED 示例入门；体验本项目时使用本页的 **TiRTC 固件与工程参数**，无需修改 LED 示例开关。

| 官方资源 | 链接 |
| --- | --- |
| SDK 开源项目与编译说明 | [SDK 源码](https://github.com/lierda-iot/CAT1.bis_OpenCPU) · [SDK 介绍与开发说明](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/general/quick_start.html) |
| 烧录工具 | [下载工具 ZIP](https://opendocs.lierda.com/fs/lierda_upgrade_tool/lierda_upgrade_tool_latest.zip) · [软件使用与烧录教程](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/tools/flash/flash-tool.html) |
| USB 驱动 | [驱动下载与安装教程](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/tools/usb/USB_Driver_Installation_Guide.html) |

## 4. 烧录并体验 TiRTC

1. **下载固件**：[TiRTC 当前完整烧录包（约 2.94 MiB）](firmware/L_CT4IT00_YP00W_01_V04_NT26F6D0_01.binpkg)。2026-09-15 从开发仓库源码提交 `8bee220bf88280e1a344c5006ab0245b4556a115` 编译，本仓库同步了相同的应用源码。已包含底包、应用及多人对讲功能，只需这一个 `.binpkg`，适用于上述硬件配置。
2. **烧录设备**：在利尔达烧录工具中选择“自定义镜像”，载入该文件进行**全量烧录**，具体操作见第 3 节的烧录教程。该文件不是 OTA 升级包。
3. **联网绑定**：接好 SIM 卡与天线后上电，等待 OLED 显示六位绑定码，在[设备演示平台](https://xiaotai.chat/)登录并添加设备，绑定成功后设备进入主页。
4. **开始对讲**：按 `KEY0` 选择、`KEY1` 确认、`KEY2` 返回。使用 AI 前先配置并绑定平台智能体；使用 WX 时，在微信搜索“探鸽小钛”，登录同一账号并完成设备的 VoIP 授权与上报。详细步骤见[当前设备操作](examples/L_CT4IT00_YP00W_01_V04/tirtc/docs/当前设备操作.md)。

**全量烧录可能清除绑定及文件系统数据，请先备份，烧录期间不要断电。**

## 5. 从源码接入与二次开发

本仓库保留完整利尔达 OpenCPU SDK 的组件、各模组底包、示例和工具，并在 V04 项目中接入 TiRTC。下列命令默认构建 TiRTC；其他官方示例按各自说明选择项目和参数。本次验证范围为 NT26F6D0 / F6D_A 的 TiRTC 应用。官方 SDK 说明见 [中文](README_ZH.md) / [English](README_SDK.md)。

在仓库根目录执行 Windows 编译命令：

```powershell
.\build.bat all PROJECT=L_CT4IT00_YP00W_01_V04 MODEM=NT26F6D0 MODEMPKG=F6D_A
```

生成固件：`gccout/L_CT4IT00_YP00W_01_V04/L_CT4IT00_YP00W_01_V04_NT26F6D0_01.binpkg`。烧录方法同上；仓库的 `firmware/` 保存已发布的完整烧录包，不会随个人本地编译自动更新，也不提交编译中间产物。修改功能或日志开关后，应使用新的构建输出目录，示例见[应用构建说明](examples/L_CT4IT00_YP00W_01_V04/tirtc/README.md#默认构建)。

若使用利尔达工具从源码编译，选择以下参数（路径相对于仓库根目录）：

| 配置项 | 选择 |
| --- | --- |
| 应用文件夹 | `examples/L_CT4IT00_YP00W_01_V04`（不是下一级 `tirtc` 目录） |
| 底包路径 | `components/basePkg/F6D_A` |
| 模组型号 | `NT26F6D0` |

TiRTC 应用位于 [examples/L_CT4IT00_YP00W_01_V04/tirtc](examples/L_CT4IT00_YP00W_01_V04/tirtc/)：利尔达 SDK 提供 4G、系统和硬件能力，应用完成绑定与呼叫信令，TiRTC 负责实时连接及音频、命令传输。

- [当前设备操作](examples/L_CT4IT00_YP00W_01_V04/tirtc/docs/当前设备操作.md)：绑定、按键、AI 呼叫、网页创建/加入多人房间及 OLED 状态。
- [AI 呼叫微信与设备配置](examples/L_CT4IT00_YP00W_01_V04/tirtc/docs/AI呼叫微信与设备配置详解.md)：联系人准备、智能体和设备插件填写示例。
- [代码与流程详解](examples/L_CT4IT00_YP00W_01_V04/tirtc/docs/代码与流程详解.md)：文件职责、模块架构、AI 转呼和多人对讲业务流程。
- [TiRTC 接口调用与模块流程](examples/L_CT4IT00_YP00W_01_V04/tirtc/docs/TiRTC接口调用与模块流程.md)：SDK 调用位置、参数与回调，以及 AI 呼叫命令和多人房间信令。

[TiRTC 官方文档](https://docs.tange.ai/products/tirtc/) · [WebRTC / TiRTC 基础知识](https://github.com/tangeai/xiaotai-esp32/blob/main/docs/README_CN.md)

应用源码、利尔达 SDK、TiRTC SDK 及第三方组件分别遵循各自授权声明。
