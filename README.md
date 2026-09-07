# TiRTC × 利尔达 OpenCPU

探鸽智能基于利尔达 OpenCPU SDK 开发的 4G 语音设备工程，接入 TiRTC，实现 AI 对话、微信对讲、设备互呼和远端语音对讲。

当前应用面向 **NT26F6D0 / F6D_A**，使用 **SSD1306 OLED、三按键和 ES8311 音频**。

本仓库保留当前 TiRTC 应用及其编译所需的 SDK、底包和工具；其他模组与示例请参考下方利尔达官方仓库。应用源码、SDK 和底包分别遵循各自授权声明。

## 主要功能

| 功能 | 简介 |
| --- | --- |
| **AI 语音对话** | 语音提问、AI 语音回复；采用半双工，设备与 AI 轮流说话。 |
| **WX 微信对讲** | 设备与微信小程序“探鸽小钛”之间双向呼叫、接听和语音对讲。 |
| **DEV 设备互呼** | 设备之间发起呼叫、接听、实时语音对讲和挂断。 |
| **LIVE 远端对讲** | 平台远端接入设备进行语音对讲；仅设备停留在首页且空闲时允许接入，离开首页后结束。 |
| **SET 音频设置** | 调整扬声器音量和麦克风等级。 |
| **联网、绑定与显示** | 4G 联网、设备绑定；通过三按键操作，OLED 显示网络、业务状态和时间。 |

当前工程为音频应用，不提供视频画面；各对讲功能共用连接和音频资源，同一时间运行一个媒体会话。

## TiRTC 接入与体验

设备通过 4G 联网，使用 HTTP/MQTT 完成绑定、凭据获取和呼叫信令，再通过 TiRTC 建立实时连接、传输音频与命令。麦克风采集、编解码和扬声器播放由本地音频层负责，AI 对话由平台服务提供。

未绑定设备可根据 OLED 六位码，在[演示平台](https://demo-open.tange-ai.com/)完成绑定。AI 智能体配置、微信账号授权、按键与界面说明见《当前设备操作》。

## 代码与文档

应用代码：[examples/L_CT4IT00_YP00W_01_V04/tirtc](examples/L_CT4IT00_YP00W_01_V04/tirtc/)。

- [当前设备操作](examples/L_CT4IT00_YP00W_01_V04/tirtc/docs/当前设备操作.md)：绑定、按键、对讲及 OLED 状态。
- [代码与流程详解](examples/L_CT4IT00_YP00W_01_V04/tirtc/docs/代码与流程详解.md)：文件职责、模块架构和业务流程。
- [TiRTC 接口调用与模块流程](examples/L_CT4IT00_YP00W_01_V04/tirtc/docs/TiRTC接口调用与模块流程.md)：SDK 调用位置、参数、回调和连接回收。

## 编译与烧录

### 下载现成固件

[下载 TiRTC 完整烧录包（约 2.90 MiB）](https://github.com/tangeai/xiaotai-lierda/raw/refs/heads/main/firmware/L_CT4IT00_YP00W_01_V04_NT26F6D0_01.binpkg)，适用于本工程的 **NT26F6D0 / F6D_A** 硬件配置。

该 `.binpkg` 已包含底包和 TiRTC 应用，直接烧录只需这一个文件，无需另外下载底包或应用 `.bin`。使用支持“自定义镜像”的利尔达烧录工具，选择该文件进行**全量烧录**；它不是 OTA 升级包。操作见下方官方烧录教程。

### 源码编译

在仓库根目录执行当前 TiRTC 工程的 Windows 编译命令：

```powershell
.\build.bat all PROJECT=L_CT4IT00_YP00W_01_V04 MODEM=NT26F6D0 MODEMPKG=F6D_A
```

产物目录：`gccout/L_CT4IT00_YP00W_01_V04/`。当前底包与应用组合固件为 `L_CT4IT00_YP00W_01_V04_NT26F6D0_01.binpkg`。

仓库的 `firmware/` 只提供这一个完整烧录包；`gccout/` 中其余中间产物及解压后的工具链不提交。

从源码工程烧录时，利尔达工具使用以下配置（直接烧录上述现成 `.binpkg` 时使用“自定义镜像”）：

| 配置项 | 选择 |
| --- | --- |
| 应用文件夹 | `<仓库根目录>/examples/L_CT4IT00_YP00W_01_V04`，不是下一级 `tirtc` 目录 |
| 底包路径 | `<仓库根目录>/components/basePkg/F6D_A` |
| 模组型号 | `NT26F6D0` |

环境搭建、Linux 编译、驱动安装和烧录操作直接参考利尔达官方资料；通用示例中的工程参数以本页为准：

| 官方资源 | 链接 |
| --- | --- |
| SDK 开源项目与编译说明 | [SDK 源码](https://github.com/lierda-iot/CAT1.bis_OpenCPU) · [SDK 介绍与开发说明](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/general/quick_start.html) · [快速上手教程](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/general/quick_run_demo.html) |
| 烧录工具 | [下载工具 ZIP](https://opendocs.lierda.com/fs/lierda_upgrade_tool/lierda_upgrade_tool_latest.zip) · [软件使用与烧录教程](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/tools/flash/flash-tool.html) |
| USB 驱动 | [驱动下载与安装教程](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/tools/usb/USB_Driver_Installation_Guide.html) |

**全量烧录可能清除绑定及文件系统数据，请先备份，烧录期间不要断电。**

## 基础知识与延伸阅读

首次接触实时音视频，可参考[小钛 ESP32 开发者指南](https://github.com/tangeai/xiaotai-esp32/blob/main/docs/README_CN.md)：

1. [WebRTC 是什么](https://github.com/tangeai/xiaotai-esp32/blob/main/docs/01_WEBRTC_OVERVIEW_CN.md)：跨网连接与实时传输。
2. [TiRTC 的设计与快速体验](https://github.com/tangeai/xiaotai-esp32/blob/main/docs/02_TIRTC_OVERVIEW_AND_EXPERIENCE_CN.md)：设备身份、授权与体验流程。
3. [具体代码实现](https://github.com/tangeai/xiaotai-esp32/blob/main/docs/03_TIRTC_DEVELOPMENT_CN.md)：SDK 生命周期、连接和异步回调。
4. [音视频调试](https://github.com/tangeai/xiaotai-esp32/blob/main/docs/04_AUDIO_VIDEO_PIPELINE_CN.md)：采集、缓冲、编解码与播放链路。

上述资料用于理解通用原理；利尔达工程的硬件、SDK 版本和媒体参数以本仓库为准。

## 相关入口

[TiRTC 官方文档](https://docs.tange.ai/products/tirtc/) · [设备演示平台](https://demo-open.tange-ai.com/) · [利尔达官方文档](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/general/index.html)

利尔达 SDK、TiRTC SDK 及第三方组件遵循各自授权声明。
