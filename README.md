# TiRTC × 利尔达 OpenCPU

基于利尔达 OpenCPU 的小钛 4G 终端示例，通过 **TiRTC** 实现 AI 对话、微信与设备通话、远程对讲。选择你的产品，跟着演示和操作说明开始体验。

## 选择你的产品

### lite · OLED 语音终端

128×64 OLED、三个按键，支持 AI 语音对话、AI 呼叫联系人、微信与设备语音电话、远端语音对讲和独立多人房间。音量与麦克风增益重启恢复默认。

**[进入 lite：看演示、准备硬件、逐步上手 →](examples/L_CT4IT00_YP00W_01_V04/tirtc_lite/README.md)**

### pro · 触屏音视频终端

320×240 触屏与摄像头，支持 AI 对话和字幕、AI 呼叫联系人、微信与设备音视频电话、摄像头实时查看和远程对讲。支持扬声器音量、麦克风增益与音频开关记忆；支持房间创建/加入、成员列表和按住讲话的多人对讲。

**[进入 pro：看演示、准备硬件、逐步上手 →](examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/README.md)**

## 编译与下载

也可以直接下载最新固件，按对应入门页完成烧录。需要从源码编译时，在本地磁盘的短英文路径（例如 `C:\dev`，避免空格和中文）打开 Windows PowerShell，获取完整仓库：

```powershell
git clone -c core.longpaths=true https://github.com/tangeai/xiaotai-lierda.git
cd xiaotai-lierda
```

也可下载完整仓库 ZIP 并解压后进入根目录；不要只下载产品子目录。仓库已包含 Windows 编译工具和 GCC 压缩包，首次编译会自动解压到 `tools/toolchain/gcc`，无需另外安装 ARM 编译器。pro 还需按 [Python 环境准备](examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/docs/当前设备操作.md#python-setup)安装 Python。准备完成后按产品执行：

| 产品 | 编译命令 | 最新固件 |
| --- | --- | --- |
| lite | `.\build_tirtc.bat lite` | [下载 lite 最新固件](firmware/lite/latest.binpkg) |
| pro | `.\build_tirtc.bat pro` | [下载 pro 最新固件](firmware/pro/latest.binpkg) |

pro 最新固件为出厂初始化包，包含固件、外置 Flash 字库和背景，**一次烧录即可；会覆盖外置 Flash 文件，请先备份需要保留的文件。** 板型要求、烧录步骤和验证范围见 [pro 入门指南](examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/docs/当前设备操作.md#build-flash)。

Pro 当前发布名统一为 `pro_release`，最新下载包包含本次多人对讲与音频修复；后续用 Git 提交和固件 SHA-256 区分构建。操作见 [pro 多人对讲](examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/docs/多人对讲.md)，包内容和验证范围见 [pro 发布记录](release/tirtc_pro/README.md)。

## 从使用到开发

两套产品共用公共 SDK 源码和工具链，应用代码、配置与编译输出各自独立。按产品入门页的“操作 → 架构 → TiRTC 接入 → AI 呼叫配置”继续阅读：[lite 代码架构](examples/L_CT4IT00_YP00W_01_V04/tirtc_lite/docs/代码与流程详解.md) / [pro 代码架构](examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/docs/代码与流程详解.md)。

## 官方资料

- [TiRTC 产品与接入文档](https://docs.tange.ai/products/tirtc/)
- [小钛体验平台](https://xiaotai.chat/)
- [利尔达快速上手](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/general/quick_run_demo.html)
- [利尔达文档首页](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/)
- 本仓库保留的 [SDK 英文说明](README_SDK.md) / [SDK 中文说明](README_ZH.md)

探鸽自有的 lite / pro 应用代码与文档采用 [MIT 许可证](LICENSE)。利尔达 OpenCPU SDK、原厂派生代码、TiRTC 预编译 SDK、第三方组件及字体资源保留各自授权，具体范围见 [许可与第三方说明](THIRD_PARTY_NOTICES.md)。
