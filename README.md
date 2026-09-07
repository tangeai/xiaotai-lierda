# TiRTC × 利尔达 OpenCPU

探鸽智能基于利尔达 OpenCPU SDK 开发的 4G 语音设备开源工程，通过 **TiRTC** 实现 AI 对话、微信对讲、设备互呼和远端语音对讲。

## 1. 先看设备能做什么

| 功能 | 使用体验 | 操作视频 |
| --- | --- | --- |
| 联网与绑定 | 4G 联网后，点击[绑定设备](https://demo-open.tange-ai.com/devices)，登录后输入 OLED 六位绑定码，绑定成功后设备进入主页。 | [设备绑定](#video-binding) |
| AI 语音对话 | 向 AI 提问并听取语音回复，设备与 AI 轮流说话（半双工）。 | [AI 对讲](#video-ai-talk) |
| WX 微信对讲 | 与微信小程序“探鸽小钛”双向呼叫、接听和语音对讲。 | [微信互叫](#video-wechat-call) |
| DEV 设备互呼 | 从联系人列表选择已联网、绑定的设备，呼叫、接听、对讲和挂断。 | [设备互叫](#video-device-call) |
| SET 音频设置 | 调整扬声器音量和麦克风增益，重启后恢复默认。 | [音量设置](#video-audio-settings) |
| LIVE 远端对讲 | 网页远端接入；仅设备在主页且空闲时允许，离开主页后结束。 | [操作说明（第 10 章）](examples/L_CT4IT00_YP00W_01_V04/tirtc/docs/当前设备操作.md) |

设备通过三个按键操作，OLED 显示菜单、网络状态和时间。当前仅传输音频，同一时间运行一个媒体会话。

### 操作视频

点击标题展开视频；播放器默认静音，可点击音量按钮开启声音。

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

1. **下载固件**：[TiRTC 完整烧录包（约 2.90 MiB）](https://github.com/tangeai/xiaotai-lierda/raw/refs/heads/main/firmware/L_CT4IT00_YP00W_01_V04_NT26F6D0_01.binpkg)。已包含底包和应用，只需这一个 `.binpkg`，适用于上述硬件配置。
2. **烧录设备**：在利尔达烧录工具中选择“自定义镜像”，载入该文件进行**全量烧录**，具体操作见第 3 节的烧录教程。该文件不是 OTA 升级包。
3. **联网绑定**：接好 SIM 卡与天线后上电，等待 OLED 显示六位绑定码，在[设备演示平台](https://demo-open.tange-ai.com/)登录并添加设备，绑定成功后设备进入主页。
4. **开始对讲**：按 `KEY0` 选择、`KEY1` 确认、`KEY2` 返回。使用 AI 前先配置并绑定平台智能体；使用 WX 时，在微信搜索“探鸽小钛”，登录同一账号并完成设备的 VoIP 授权与上报。详细步骤见[当前设备操作](examples/L_CT4IT00_YP00W_01_V04/tirtc/docs/当前设备操作.md)。

**全量烧录可能清除绑定及文件系统数据，请先备份，烧录期间不要断电。**

## 5. 从源码接入与二次开发

本仓库已包含 TiRTC 应用及其编译所需的利尔达 SDK、F6D_A 底包和工具。在仓库根目录执行 Windows 编译命令：

```powershell
.\build.bat all PROJECT=L_CT4IT00_YP00W_01_V04 MODEM=NT26F6D0 MODEMPKG=F6D_A
```

生成固件：`gccout/L_CT4IT00_YP00W_01_V04/L_CT4IT00_YP00W_01_V04_NT26F6D0_01.binpkg`。烧录方法同上；仓库的 `firmware/` 仅保留一份完整烧录包，不提交编译中间产物。

若使用利尔达工具从源码编译，选择以下参数（路径相对于仓库根目录）：

| 配置项 | 选择 |
| --- | --- |
| 应用文件夹 | `examples/L_CT4IT00_YP00W_01_V04`（不是下一级 `tirtc` 目录） |
| 底包路径 | `components/basePkg/F6D_A` |
| 模组型号 | `NT26F6D0` |

TiRTC 应用位于 [examples/L_CT4IT00_YP00W_01_V04/tirtc](examples/L_CT4IT00_YP00W_01_V04/tirtc/)：利尔达 SDK 提供 4G、系统和硬件能力，应用完成绑定与呼叫信令，TiRTC 负责实时连接及音频、命令传输。

- [当前设备操作](examples/L_CT4IT00_YP00W_01_V04/tirtc/docs/当前设备操作.md)：绑定、按键、对讲及 OLED 状态。
- [代码与流程详解](examples/L_CT4IT00_YP00W_01_V04/tirtc/docs/代码与流程详解.md)：文件职责、模块架构和业务流程。
- [TiRTC 接口调用与模块流程](examples/L_CT4IT00_YP00W_01_V04/tirtc/docs/TiRTC接口调用与模块流程.md)：SDK 调用位置、参数与回调。

[TiRTC 官方文档](https://docs.tange.ai/products/tirtc/) · [WebRTC / TiRTC 基础知识](https://github.com/tangeai/xiaotai-esp32/blob/main/docs/README_CN.md)

应用源码、利尔达 SDK、TiRTC SDK 及第三方组件分别遵循各自授权声明。
