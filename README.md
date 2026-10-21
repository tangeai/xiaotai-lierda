# TiRTC × 利尔达 OpenKit

基于利尔达 OpenKit 的小钛 4G 终端示例，通过 **TiRTC** 实现 AI 对话、微信与设备通话、远程对讲。选择你的产品，跟着演示和操作说明开始体验。

> **首次烧录先看：[Lite / Pro 简明全量烧录图解（含两张操作截图）](examples/NT26FxDx_OpenKit/docs/全量烧录.md)**
>
> 下载配套固件后按图操作。Lite 全量下载一次；Pro 首次安装先资源包、再正式应用包。官方工具与详细教程保留在图解末尾及下方资料区。

## 选择你的产品

### lite · OLED 语音终端

128×64 OLED、三个按键，支持 AI 语音对话、AI 呼叫联系人、微信与设备语音电话、远端语音对讲和独立多人房间。音量与麦克风增益重启恢复默认。

**[进入 lite：看演示、准备硬件、逐步上手 →](examples/NT26FxDx_OpenKit/tirtc_lite/README.md)**

### pro · 触屏音视频终端

320×240 触屏与摄像头，支持 AI 对话和字幕、AI 呼叫联系人、微信与设备音视频电话、摄像头实时查看和远程对讲。支持扬声器音量、麦克风增益与音频开关记忆；支持房间创建/加入、成员列表和按住讲话的多人对讲。

**[进入 pro：看演示、准备硬件、逐步上手 →](examples/NT26FxDx_OpenKit/tirtc_pro/README.md)**

## 编译与下载

也可以直接下载最新固件，按对应入门页完成烧录。需要从源码编译时，在本地磁盘的短英文路径（例如 `C:\dev`，避免空格和中文）打开 Windows PowerShell，获取完整仓库：

```powershell
git clone -c core.longpaths=true https://github.com/tangeai/xiaotai-lierda.git
cd xiaotai-lierda
```

也可下载完整仓库 ZIP 并解压后进入根目录；不要只下载产品子目录。仓库已包含 Windows 编译工具和 GCC 压缩包，首次编译会自动解压到 `tools/toolchain/gcc`，无需另外安装 ARM 编译器。pro 还需按 [Python 环境准备](examples/NT26FxDx_OpenKit/tirtc_pro/docs/当前设备操作.md#python-setup)安装 Python 3.10 或更高版本。准备完成后按产品执行：

| 产品 | 编译命令 | 最新固件 |
| --- | --- | --- |
| lite | `.\examples\NT26FxDx_OpenKit\tirtc_lite\build.bat` | [下载 lite 最新固件](firmware/lite/) |
| pro | `.\examples\NT26FxDx_OpenKit\tirtc_pro\build.bat` | [下载 pro 最新固件](firmware/pro/) |

pro 使用官方原版工具时，推荐按顺序烧录两个配套包：**先烧录 `_pro_assets.binpkg` 资源安装包，运行并确认 `INSTALL_OK`，再烧录 `_pro_app.binpkg` 正式应用包。** 两次均选择“自定义镜像 → 全量下载”；烧录前备份绑定、设置和需要保留的文件。包的选择、成功日志及故障处理见 [pro 两次烧录说明](firmware/pro/README.md)，板型要求见 [pro 入门指南](examples/NT26FxDx_OpenKit/tirtc_pro/docs/当前设备操作.md#build-flash)。

固件按对应应用或资源安装程序的编译完成时间（UTC+8）命名，分别提供 [lite 校验清单](firmware/lite/SHA256SUMS.txt) / [pro 校验清单](firmware/pro/SHA256SUMS.txt)。pro 的 `_pro.binpkg` 是包含外置资源镜像的合并包，只有已验证支持本板外置 Flash 写入的工具才能使用，官方原版工具请按上述两次烧录流程选择文件。操作见 [pro 多人对讲](examples/NT26FxDx_OpenKit/tirtc_pro/docs/多人对讲.md)，包内容和构建方式见 [pro 构建与固件](examples/NT26FxDx_OpenKit/tirtc_pro/docs/构建与固件.md)。

## 从使用到开发

两套产品共用公共 SDK 源码和工具链，应用代码、配置与编译输出各自独立。当前使用官方 SDK 1.7.0 和 `F6D_D` 底包，默认开启利尔达 AEC；AI 对话支持边播边听与插话打断。按产品入门页的“操作 → 架构 → TiRTC 接入 → AI 呼叫配置”继续阅读：[lite 代码架构](examples/NT26FxDx_OpenKit/tirtc_lite/docs/代码与流程详解.md) / [pro 代码架构](examples/NT26FxDx_OpenKit/tirtc_pro/docs/代码与流程详解.md)。

## 官方资料

- [TiRTC 产品与接入文档](https://docs.tange.ai/products/tirtc/)
- [小钛体验平台](https://xiaotai.chat/)
- [利尔达快速上手](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/general/quick_run_demo.html)
- [利尔达完整烧录教程](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/tools/flash/flash-tool.html)
- [利尔达文档首页](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/)
- 本仓库保留的 [SDK 英文说明](README_SDK.md) / [SDK 中文说明](README_ZH.md)

探鸽自有的应用代码与文档采用 MIT 许可证（[lite](examples/NT26FxDx_OpenKit/tirtc_lite/LICENSE) / [pro](examples/NT26FxDx_OpenKit/tirtc_pro/LICENSE)）。利尔达 OpenKit SDK、原厂派生代码、TiRTC 预编译 SDK、第三方组件及字体资源保留各自授权，具体范围见 [许可与第三方说明](THIRD_PARTY_NOTICES.md)。
