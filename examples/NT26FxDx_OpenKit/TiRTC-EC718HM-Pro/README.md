# HM Pro · TiRTC × 利尔达 OpenKit 触屏音视频示例

[返回产品选择](../../../README.md) · [探鸽小钛项目主页](https://github.com/tangeai/xiaotai)

探鸽小钛 HM Pro 是基于利尔达 NT26F9D0（EC718HM）OpenKit 的开源 4G 触屏音视频固件。使用 320×240 触屏和 GC032A 摄像头，支持 AI 语音和字幕、微信与设备双向视频通话、网页实时查看与远程对讲，以及多人语音对讲。

> **首次烧录先看 [HM Pro 固件与烧录](../firmware/hm-pro/README.md)**
>
> 从 [HM Pro 固件目录](../firmware/hm-pro/) 下载对应 NT26F9D0 的完整包。界面资源随应用打包，使用 OpenKit 1.7.0 自带的 `F9D_D`。

**阅读顺序** [认识功能](#features) → [准备硬件](#hardware) → [快速烧录](#lierda-guides) → [烧录与体验](#run) → [源码接入与开发](#development)。

<a id="features"></a>

## 1. 先看设备能做什么

| 功能 | 从哪里操作 | 当前实现 | 操作说明 |
| --- | --- | --- | --- |
| 联网与绑定 | 开机按屏幕提示获取绑定码 | 4G 联网、小钛平台绑定 | [联网与绑定](docs/当前设备操作.md#binding) |
| AI 语音对话 | 点击首页表情 | 语音对话和字幕 | [AI 语音对话](docs/当前设备操作.md#ai) |
| AI 呼叫联系人 | 在 AI 对话中说出联系人 | 配置设备插件后，可指定语音或视频 | [AI 呼叫配置](docs/AI呼叫微信与设备配置详解.md) |
| 微信和设备音视频通话 | 通讯录或来电页 | 语音及双向视频、接听与拒绝、开关麦克风和摄像头 | [通话操作](docs/当前设备操作.md#calls) |
| 实时查看与远程对讲 | 从小钛网页端发起 | 查看设备画面并语音对讲 | [实时查看](docs/当前设备操作.md#live) |
| 多人对讲 | 菜单 → 多人对讲 | 创建或加入房间、按住讲话 | [多人对讲](docs/当前设备操作.md#group) |
| 声音设置与状态 | 设置、网络信息、运行状态 | 调节音量和麦克风增益，查看网络与设备信息 | [设置与状态](docs/当前设备操作.md#settings) |

微信视频使用 240×240 MJPEG 配置，设备屏幕为 320×240。多人房间使用语音。

<a id="hardware"></a>

## 2. 准备硬件与账号

- 硬件包括 NT26F9D0 视频板、配套触屏与 GC032A 摄像头，以及可传输数据的 USB 线。 [板级配置](boards/f9d_d/README.md)
- 联网需要已开通数据业务的 SIM 卡和 4G 主天线。用小钛账号绑定设备，再在微信小程序中完成设备授权。 [小钛设备页](https://xiaotai.chat/devices)

<a id="lierda-guides"></a>

## 3. 快速烧录与官方资料

打开 [HM Pro 网页烧录](https://xiaotai.chat/web-flasher/?board=lierda-ec718hm-pro)，选择本板完整包并按页面提示操作。首次安装或换底包时，先备份设备并确认要写入的范围。 [HM Pro 烧录步骤](../firmware/hm-pro/README.md#flash) · [利尔达快速上手](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/general/quick_run_demo.html) · [原厂烧录教程](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/tools/flash/flash-tool.html)

<a id="run"></a>

## 4. 烧录并体验 TiRTC

### 4.1 选择固件，或从源码编译

从固件目录选取 `YYYY_MM_DD_HH_MM_SS_hm_pro.binpkg` 并按同目录的 `SHA256SUMS.txt` 核对。也可以自行构建。 [固件下载与校验](../firmware/hm-pro/README.md#download) · [构建与固件](docs/构建与固件.md)

### 4.2 烧录完整包

使用原厂工具时，选择同一个完整包。已安装匹配的 `F9D_D` 1.7.0 和原生分区时，可仅更新 APP。首次安装或从旧布局迁移时，先备份整片 Flash，核对底包、应用和文件系统的写入范围。 [原厂烧录步骤](../firmware/hm-pro/README.md#flash)

### 4.3 开机并体验

1. 检查触屏和 4G 注册，按屏幕提示完成绑定。 [联网与绑定](docs/当前设备操作.md#binding)
2. 从首页进入 AI 对话，检查语音与字幕。 [AI 语音对话](docs/当前设备操作.md#ai)
3. 从通讯录尝试微信或设备通话，再体验网页实时查看和多人语音对讲。 [设备操作](docs/当前设备操作.md)

<a id="development"></a>

## 5. 从源码接入与二次开发

在 Windows 上使用与 Lite、PM Pro 相同的 OpenKit SDK 1.7.0 目录。在应用仓库中运行 `python tools/setup_sdk.py --product hm-pro`，准备脚本使用 OpenKit 自带的 `F9D_D` 并获取 TiRTC 2.5.0 mini。然后在这个 SDK 目录中执行本产品的 `build.bat`。 [准备与构建命令](docs/构建与固件.md)

| 内容 | 文档 |
| --- | --- |
| 准备 SDK、编译和烧录 | [构建与固件](docs/构建与固件.md) |
| 下载和校验发布固件 | [固件目录说明](../firmware/hm-pro/README.md) |
| 产品功能和操作入口 | [当前设备操作](docs/当前设备操作.md) |
| 代码布局与业务流程 | [代码与流程详解](docs/代码与流程详解.md) |
| 平台插件接入 | [AI 呼叫配置](docs/AI呼叫微信与设备配置详解.md) |

## 参考与许可

应用代码采用 [MIT 许可](LICENSE)。原厂派生代码、TiRTC SDK 和第三方资源保留各自的条款。 [许可与第三方说明](THIRD_PARTY_NOTICES.md)
