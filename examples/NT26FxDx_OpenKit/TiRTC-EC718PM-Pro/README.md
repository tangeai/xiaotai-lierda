# pro · TiRTC × 利尔达 OpenKit 触屏语音示例

[返回产品选择](../../../README.md) · [探鸽小钛项目主页](https://github.com/tangeai/xiaotai)

探鸽小钛 Pro 是基于利尔达 OpenKit 的开源 4G 触屏语音固件。通过 **TiRTC**，设备支持 AI 语音对话、AI 呼叫联系人、微信语音通话、设备间语音通话、网页远程对讲和多人语音对讲。

> **首次烧录先看：[简明全量烧录图解（含两张操作截图）](../docs/全量烧录.md)**
>
> [下载本版完整语音固件](../firmware/pro/README.md)。原厂工具首次安装按“资源安装包 → `INSTALL_OK` → 语音应用包”执行。

本项目基于 **L-CT4IT00-YP00W-01-V04 / NT26F6D0（EC718PM）**，工程目录为 `NT26FxDx_OpenKit`，展示 TiRTC 如何接入这套 OpenKit 硬件。先按本文把设备运行起来，再沿文档读懂平台接入、媒体传输和应用代码，最后替换成自己的产品界面与业务。

<p>
  <img src="docs/assets/v4-guide/home.png" width="320" alt="当前源码渲染：表情首页">
  <img src="docs/assets/v4-guide/menu.png" width="320" alt="当前源码渲染：更多功能">
</p>

图为**当前 UI 源码的离线渲染**，使用工程内的字体、图标、背景和布局，原生尺寸 320×240；时间、联系人等为示例数据。不是硬件拍照，也不代表真实连接状态。左右箭头切换表情/时钟，右上角 `···` 打开菜单。

**阅读顺序：** [认识功能](#features) → [看演示](#demo-videos) → [准备硬件](#hardware) → [简明烧录图解](../docs/全量烧录.md) → [烧录与体验](#run) → [源码接入与开发](#development)；[官方资料](#lierda-guides)按需查阅。

> 当前构建版本为 `pro_release`，使用官方 SDK 1.7.0 / `F6D_D`，默认启用 AEC 和 ANS 噪声抑制（与 Lite 相同的 `bypass=0, mode=1`），AGC 保持关闭；录放音增益曲线与 Lite 对齐，出厂默认 L8，已保存档位优先。AI 与设备互呼使用 Opus / 16 kHz 单声道，上行 20 ms、目标 24 kbit/s。AI 回复期间暂停麦克风上传。多人对讲见 [图文操作](docs/多人对讲.md#quick-start)，固件用途和构建方式见 [构建说明](docs/构建与固件.md)。

<a id="features"></a>
## 1. 先看设备能做什么

| 功能 | 从哪里操作 | 当前实现 | 演示 / 操作说明 |
| --- | --- | --- | --- |
| 联网与绑定 | 首次开机按屏幕提示 | 4G 自动联网、六位码绑定、重启恢复身份、断线恢复 | [观看演示](#video-binding) |
| AI 语音对话 | 点首页表情 | AEC 回声消除、半双工语音、KEY2 打断或双按退出、字幕 | [观看演示](#video-ai-talk) |
| AI 呼叫联系人 | 对 AI 说“呼叫小李” | 发起语音电话，需配置云端设备插件 | [观看演示](#video-ai-call) |
| 微信语音电话 | 通讯录 → 微信联系人 | 语音呼出（正式版小程序，`wx_version_type=0`）、接听/拒绝、开麦/关麦、挂断 | [操作说明](docs/当前设备操作.md#calls) |
| 设备间语音电话 | 通讯录 → 设备联系人 | 同步平台联系人，向另一台已绑定、在线设备发起通话 | [操作说明](docs/当前设备操作.md#device-call) |
| 远程对讲 | 从平台发起 | 收听设备麦克风，按住说话向设备发送语音 | [操作说明](docs/当前设备操作.md#live) |
| 多人对讲 | 菜单 → 多人对讲 | 创建/加入房间、成员列表、按住讲话、退出、电话抢占恢复；固定 G.711A 窄带语音 | [观看演示](#video-group-intercom) · [完整说明](docs/多人对讲.md) |
| 声音设置与记忆 | 设置页、KEY0/KEY1 | 扬声器音量、麦克风增益下拉选项、麦克风/扬声器开关、内部 NVM 保存 | [操作说明](docs/当前设备操作.md#settings) |
| 状态查看 | 菜单 → 网络信息/运行状态 | 注册状态、无线质量、内存与存储、运行事件 | [操作说明](docs/当前设备操作.md) |

### 开始前了解这些边界

- 一次保持 **一路媒体会话**；AI、电话、远程对讲和多人对讲共享连接和音频硬件。
- 微信和设备互呼使用语音，旧版视频邀请也按语音接听。首页绿色电话快捷呼叫通讯录中的第一位微信联系人。
- 远程对讲由平台发起。设备显示对讲状态，可在首页或时钟页结束对讲。
- 多人对讲从菜单进入，默认收听，按住才发送，讲话时仍处理下行。返回菜单暂停媒体但保留房间关系，确认“退出房间”才提交退房。电话抢占后，若进入通话前在房间页或房间表单，结束时返回房间页并同步。完整步骤、音频边界和错误码见 [多人对讲详解](docs/多人对讲.md)。
- 本例由触摸开始 AI，没有语音唤醒。AI 回复播放期间暂停上传麦克风，回复播完后恢复收音，仍受麦克风开关控制。

<a id="demo-videos"></a>
### 操作演示视频

新增 [多人对讲实机演示](#video-group-intercom)，可配合 [创建、加入与双端对讲步骤](docs/多人对讲.md#quick-start) 体验。

以下 4 段录像展示绑定、AI 对话、AI 发起语音电话和多人对讲。展开条目后可下载对应的 MP4。

| 演示 | 时长 | 视频文件 |
| --- | --- | --- |
| 设备绑定与基本信息查看 | 01:29 | [观看演示](#video-binding) · [下载 MP4 · 18.4 MiB](docs/assets/videos/binding.mp4?raw=true) |
| AI 对话与字幕 | 01:53 | [观看演示](#video-ai-talk) · [下载 MP4 · 7.2 MiB](docs/assets/videos/ai-talk.mp4?raw=true) |
| 通过 AI 呼叫微信（语音演示） | 00:37 | [观看演示](#video-ai-call) · [下载 MP4 · 2.9 MiB](docs/assets/videos/ai-call-wechat.mp4?raw=true) |
| 多人对讲：创建、加入与房间讲话 | 01:54 | [观看演示](#video-group-intercom) · [下载原始 MP4 · 17.2 MiB](docs/assets/videos/group-intercom.mp4?raw=true) |


演示录像用于了解操作。验收以对应固件的实机效果为准。 [构建与固件](docs/构建与固件.md)

<a id="video-binding"></a>
<details open>
<summary>设备绑定与基本信息查看（01:29）</summary>

[下载 MP4（18.4 MiB）](docs/assets/videos/binding.mp4?raw=true)

从平台绑定到设备信息查看，先了解设备和平台的关系。操作步骤、资源准备及绑定失败检查见 [联网与绑定](docs/当前设备操作.md#binding)。

</details>

<a id="video-ai-talk"></a>
<details>
<summary>AI 对话与字幕（01:53）</summary>

[下载 MP4（7.2 MiB）](docs/assets/videos/ai-talk.mp4?raw=true)

观察触屏设备的 AI 对话、字幕和语音回答。如何开始、结束及检查有字幕无声音，见 [AI 对话](docs/当前设备操作.md#ai)。

</details>

<a id="video-ai-call"></a>
<details>
<summary>通过 AI 呼叫微信（语音演示）（00:37）</summary>

[下载 MP4（2.9 MiB）](docs/assets/videos/ai-call-wechat.mp4?raw=true)

这段录像展示 AI 把语音请求转成微信语音电话。配置角色和设备插件后，以真实通话页面和对端来电为准。 [AI 角色与插件配置](docs/AI呼叫微信与设备配置详解.md)

</details>

<a id="video-group-intercom"></a>
<details>
<summary>多人对讲：创建、加入与房间讲话（01:54）</summary>

[下载原始 MP4（17.2 MiB）](docs/assets/videos/group-intercom.mp4?raw=true)

展示 Pro 创建、加入房间、成员列表，以及与电脑端的房间语音交互，其中包含连接重试画面。按 [多人对讲图文步骤](docs/多人对讲.md#quick-start) 操作，音质和功能检查见 [验收步骤](docs/多人对讲.md#validation)。原始录像完整保留，在线播放副本只压缩画面，原音轨不重新编码。

</details>

<a id="hardware"></a>
## 2. 准备硬件与账号

| 项目 | 本示例要求 |
| --- | --- |
| 板卡/模组 | L-CT4IT00-YP00W-01-V04（工程名 `NT26FxDx_OpenKit`），NT26F6D0 / F6D_D；采购时核对带触屏的板型 |
| 显示与触摸 | ST7789、FT6336；当前逻辑横屏 320×240 |
| 声音 | ES8311、麦克风、喇叭 |
| 外置资源存储 | P25Q32SH 4 MiB SPI Flash，配套字库、图标与表情资源 |
| 4G | 能上网的 SIM 卡、接好的天线、稳定供电 |
| 开发电脑 | Windows 10/11、数据 USB 线、USB 驱动、利尔达下载工具；完整工程含工具链 |
| 平台与对端 | [小钛体验平台](https://xiaotai.chat)、微信联系人授权；设备通话另需一台已绑定设备 |

本示例通过 4G 联网，不提供 Wi-Fi 配网页面。不要仅根据外观相似或同为 EC718 就混用底包、引脚和固件；板级依赖说明见 [board_compat](board_compat/README.md)。

<a id="lierda-guides"></a>
<a id="3-跟随利尔达教程准备环境"></a>
## 3. 快速烧录与官方资料

打开 [PM Pro 网页烧录](https://xiaotai.chat/web-flasher/?board=lierda-ec718pm-pro)，选择 PM Pro 完整语音包并按页面提示操作。使用原厂工具时，文件选择和顺序见第 4 节。

| 参考项 | 官方资料 | 按需了解 |
| --- | --- | --- |
| 1 | [利尔达快速体验](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/general/quick_run_demo.html) | 认识下载、调试串口和开发板连接 |
| 2 | [USB 驱动安装](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/tools/usb/USB_Driver_Installation_Guide.html) | 让电脑识别模组接口，确认实际 COM 号 |
| 3 | [SDK 快速开始](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/general/quick_start.html) | 工程目录、编译环境、硬件型号选择 |
| 4 | [固件下载工具说明](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/tools/flash/flash-tool.html) | 选择镜像、进入下载、确认成功与查看日志 |

[利尔达文档首页](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/) · [官方 SDK 英文说明](https://github.com/lierda-iot/CAT1.bis_OpenCPU/blob/59f1250c4b8caad4bbf4f74167b7b39c79097f8c/README.md) · [原始 SDK 中文说明](https://github.com/lierda-iot/CAT1.bis_OpenCPU/blob/59f1250c4b8caad4bbf4f74167b7b39c79097f8c/README_ZH.md)。

**板卡应用补充：** [NT26FxDx OpenKit 应用指导（含 V04）](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/examples/NT26FxDx%20OpenKit/NT26FxDx%20OpenKit%20Application%20Guide_Rev0.1.html) · [OpenKit TiRTC 应用开发指导](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/examples/NT26FxDx%20OpenKit%20TiRTC/NT26FxDx%20OpenKit%20TiRTC%20Application%20Guide_Rev0.1.html)。原始 SDK 中的旧 V04 快速开始链接现已失效，改从这些入口查阅；原始 SDK 说明仍保留原文。官方示例的界面和业务可能与本触屏应用不同，编译选项和操作以本文为准。

官方教程中的硬件 Demo 是通用示例，本产品通过下面的 pro 脚本选择 `APP_VARIANT=pro BUILD_MODE=tirtc`。官方在线 SDK 可能继续更新，本仓库按自己的固定依赖构建，不要求先覆盖为最新公共 SDK。

<a id="run"></a>
## 4. 烧录并体验 TiRTC

### 4.1 选择固件，或从源码编译

当前使用公共官方 SDK 1.7.0 / `F6D_D`，应用位于 `examples/NT26FxDx_OpenKit/TiRTC-EC718PM-Pro`。从旧 A 底包升级时须完整烧录配套底包与 APP；编译参数和烧录步骤见 [操作说明](docs/当前设备操作.md#build-flash)。

仓库中已有的 [Pro 预编译包](../firmware/pro/) 对应早期应用。体验本版语音功能，请使用当前源码生成的应用包。先按 [源码准备](../../../README.md#编译与下载)下载利尔达官方 `Release_V0107` SDK 和底包，再下载小钛应用仓库并执行合并命令，然后完成 [Python 环境准备](docs/当前设备操作.md#python-setup)，在准备好的 SDK 根目录执行：

```powershell
.\examples\NT26FxDx_OpenKit\TiRTC-EC718PM-Pro\build.bat
```

统一入口保留内部 `APP_VERSION=pro_release`，生成包含 D 底包、应用及外置资源镜像的 `_pro.binpkg` 合并包。普通正式应用包位于 `gccout/tirtc_pro/TiRTC_pro_NT26F6D0_pro_release.binpkg`，资源安装包需另运行 `build_assets.bat` 构建，这两份 `gccout` 原始包可直接用于两次烧录；预编译发布副本使用 `_pro_app.binpkg`、`_pro_assets.binpkg` 后缀，保存在应用仓库的发布目录，构建不会自动复制或重命名为这两个副本。发布副本的时间取各自 `.bin` 的编译完成时间（UTC+8），不是重新命名的时间。设备日志中的版本字符串带有 SDK 前缀 `appVpro_release`。完整命令、产物来源和校验说明见 [构建与固件](docs/构建与固件.md)及 [发布目录说明](../firmware/pro/README.md)。

### 4.2 原厂工具备用流程（先资源，后应用）

工具按钮的最简操作见[原厂工具备用图解（含两张截图）](../docs/全量烧录.md#原厂工具备用流程)，文件选择和烧录顺序按下面执行。

1. **备份绑定、设置及需要保留的文件**；全量下载可能覆盖内部数据，不能承诺保存原设置。
2. 结束设备当前媒体会话，关闭占用串口的软件，保持 USB 连接。
3. 在下载工具中启用“自定义镜像”，先选择本次构建的 Pro 资源安装包，执行“全量下载”。
4. 下载成功后让设备启动，查看日志并等待 `[assets14] INSTALL_OK files=7 updated=... skipped=...`。此时没有正常业务界面是预期行为；工具“下载成功”不能代替资源安装成功。
5. 关闭日志串口，选择本次构建的 Pro 语音应用包，执行第二次“全量下载”。
6. 正常启动后确认 `[assets14] LOAD_OK`，检查字体、背景与触摸，再继续联网绑定。

**顺序不能反：先安装资源，后恢复正式应用。** 两次下载均写内部 Flash；第一次启动的安装程序再将七个资源文件写入外置 Flash，第二个包不携带外置文件系统镜像。安装器不会自动切换为正式应用。详细界面操作、失败处理和文件选择见 [两次烧录说明](../firmware/pro/README.md)；开发维护见 [资源说明](resource_store/README.md)。`F6D_D_base.binpkg` 不包含完整业务。

### 4.3 按这个顺序验收

```text
开机页面和触摸正常 → 4G 移动数据连接 → 平台绑定完成
→ AI 一问一答 → 通讯录语音电话 → 来电接听
→ 远程语音对讲 → 多人对讲创建/加入及双方讲话
→ 声音设置保存 → 配置 AI 拨号
```

每一步的界面、操作、预期结果、失败检查和错误码都在 [当前设备操作](docs/当前设备操作.md)。先跑通手动通话，再配置 AI 工具，能更快判断问题出在媒体还是工具调用。

<a id="development"></a>
## 5. 从源码接入与二次开发

设备身份接入见 [TiRTC 设备 ID 获取与绑定流程（Lite / Pro 通用）](../docs/TiRTC设备ID获取与绑定流程.md)，包括 ID / 密钥来源、六位码绑定、重启恢复与 SDK 启动。

本版操作与业务文档分为 **五篇**，按需逐步深入：

| 文档 | 读完可以做什么 |
| --- | --- |
| [① 当前设备操作](docs/当前设备操作.md) | 编译、烧录、绑定、按图体验；查设置记忆范围、常见故障和应用错误码 |
| [② 代码与流程详解](docs/代码与流程详解.md) | 看懂任务分工、状态机、一次通话、媒体管线和资源/性能统计 |
| [③ TiRTC 接口调用与模块流程](docs/TiRTC接口调用与模块流程.md) | 接好身份、连接、媒体、回调和回收；查流号、能力声明、SDK 错误码并移植到自己的产品 |
| [④ AI 呼叫微信与设备配置详解](docs/AI呼叫微信与设备配置详解.md) | 配置角色和 call_contact，理解语音呼叫、协议兼容、联系人匹配与失败返回 |
| [⑤ 多人对讲](docs/多人对讲.md) | 从房间创建/加入到 G.711A 音频、PTT、诊断和服务端接口 |

业务源码集中在 [`examples/NT26FxDx_OpenKit/TiRTC-EC718PM-Pro`](.)。从 [`user_main.c`](user_main.c) 看启动和动作分发，再按业务进入 `ai`、`calls`、`remote`、`group`；硬件采播在 `media/port`，SDK 生命周期在 `runtime`。

本产品与 lite 共用公共 SDK 源码和工具链，业务代码各自在独立目录内。底包与接口头文件统一来自公共官方 SDK 1.7.0 / F6D_D；`TiRTC-EC718PM-Pro/board_compat` 只保留启动、屏幕及链接适配，TiRTC 库与对应头文件在自己的 `sdk` 中。仍在使用的适配和库是构建依赖，不能当“备份”删掉；原厂 `app/demo` 保持独立。历史同步范围见 [SDK_DIFF.md](SDK_DIFF.md)。

未来可共享的是 TiRTC 连接/媒体/错误码概念及 AI 工具协议；触屏布局、GPIO、音频引脚、存储布局、实际代码路径和默认配置属于本产品，不能直接套给另一块板。更细的复用说明见 [文档目录](docs/README.md)。

本仓库已按 `TiRTC-EC718PM-Lite / TiRTC-EC718PM-Pro` 分别维护两个产品；[双产品构建与依赖](docs/README.md#two-products) 说明目录和输出隔离方式。pro 的操作说明、演示录像和设置行为只适用于本产品，lite 请从 [Lite 产品首页](../TiRTC-EC718PM-Lite/README.md) 阅读对应说明。

## 参考与许可

- [TiRTC 产品文档](https://docs.tange.ai/products/tirtc/)：实时音视频产品及接入资料。
- [小钛利尔达参考工程](https://github.com/tangeai/xiaotai-lierda)：本页采用“功能—硬件—快速烧录—开发”阅读顺序，官方资料按需查阅，操作与参数以当前触屏产品源码为准。
- [小钛 ESP32 参考工程](https://github.com/tangeai/xiaotai-esp32)：UI 与产品交互参考，不能直接照搬其硬件参数。
- [设备能力协议](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/api-reference.md)：服务端业务与能力声明参考，部署服务版本需与设备配套。
- 探鸽自有应用采用 [MIT](LICENSE)；原厂派生代码、TiRTC 预编译 SDK、第三方组件及字体保留各自许可，详见 [SDK 分发说明](sdk/README.md) 与 [应用许可范围](THIRD_PARTY_NOTICES.md)。
