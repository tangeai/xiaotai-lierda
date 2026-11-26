# TiRTC × 利尔达 OpenKit

探鸽小钛是面向带屏和无屏设备的开源 AI 音视频固件，让你的硬件支持 **AI 聊天、微信通话、设备对讲和远程实时音视频**。

![探鸽小钛功能示意（具体能力以板型与固件为准）](examples/NT26FxDx_OpenKit/docs/assets/xiaotai.gif)

基于利尔达 OpenKit 的小钛 4G 终端示例，通过 **TiRTC** 实现 AI 对话、微信与设备通话、远程对讲。选择你的产品，跟着演示和操作说明开始体验。

> **首次烧录先看：[按板型选择固件和烧录流程](examples/NT26FxDx_OpenKit/docs/全量烧录.md)**
>
> 三款板各有配套的完整 `.binpkg`。先确认模组和产品，再按对应板型的烧录说明操作。

## 选择你的产品

### TiRTC-EC718PM-Lite · OLED 语音终端

128×64 OLED、三个按键，支持 AI 语音对话、AI 呼叫联系人、微信与设备语音电话、远端语音对讲和独立多人房间。音量与麦克风增益重启恢复默认。

**[进入 lite：看演示、准备硬件、逐步上手 →](examples/NT26FxDx_OpenKit/TiRTC-EC718PM-Lite/README.md)**

### TiRTC-EC718PM-Pro · 触屏语音终端

320×240 触屏，支持 AI 对话和字幕、AI 呼叫联系人、微信与设备语音电话、网页远程对讲。支持扬声器音量、麦克风增益与音频开关记忆；支持房间创建/加入、成员列表和按住讲话的多人对讲。

**[进入 pro：看演示、准备硬件、逐步上手 →](examples/NT26FxDx_OpenKit/TiRTC-EC718PM-Pro/README.md)**

### TiRTC-EC718HM-Pro · NT26F9D0 触屏音视频终端

320×240 触屏和 GC032A 摄像头，支持 AI 语音及字幕、AI 呼叫联系人、微信与设备双向视频通话、网页实时查看与对讲，以及多人语音对讲。

**[进入 HM Pro：看功能、准备硬件、下载固件并上手 →](examples/NT26FxDx_OpenKit/TiRTC-EC718HM-Pro/README.md)**

## 编译与下载

三款产品共用一份利尔达 OpenKit SDK 1.7.0 和工具链。Lite 和 PM Pro 沿用原有 `F6D_D`，HM Pro 选择 SDK 自带的 `F9D_D`。准备 HM Pro 应用时，脚本下载并校验其 TiRTC 2.5.0 mini 配套包。 [HM Pro 构建说明](examples/NT26FxDx_OpenKit/TiRTC-EC718HM-Pro/docs/构建与固件.md)

也可以直接下载已有固件，按对应入门页完成烧录。需要从源码编译时，先安装 Git 和 Python 3.10 或更高版本，在本地磁盘的短英文路径（例如 `C:\dev`，避免空格和中文）打开 Windows PowerShell。首次使用时，先下载应用源码；已有本仓库时，直接在仓库根目录执行后续步骤。

```powershell
git clone --depth 1 -c core.longpaths=true https://github.com/tangeai/xiaotai-lierda.git xiaotai-lierda
cd xiaotai-lierda
```

```powershell
# 1. 下载利尔达官方 SDK、底包和工具链（固定 1.7.0 版本）
git clone --depth 1 --branch Release_V0107 -c core.autocrlf=false -c core.longpaths=true https://gitee.com/lierda-iot/CAT1.bis_OpenCPU.git ../CAT1.bis_OpenCPU-sdk

# 2. 准备应用并校验 SDK 版本
python tools/setup_sdk.py --sdk-dir ../CAT1.bis_OpenCPU-sdk

# 3. 逐文件检查合并结果和构建补丁
python tools/setup_sdk.py --sdk-dir ../CAT1.bis_OpenCPU-sdk --check
```

利尔达官方 `Release_V0107`（SDK 1.7.0）包含 `components/basePkg/F6D_D`、`components/basePkg/F9D_D` 和编译工具。准备脚本校验官方 SDK 提交 `59f1250c4b8caad4bbf4f74167b7b39c79097f8c`，应用构建适配，并放入 `examples/NT26FxDx_OpenKit` 对应目录。官方 Gitee 与 [GitHub](https://github.com/lierda-iot/CAT1.bis_OpenCPU) 提供该发布标签。若已有对应 SDK，可以跳过克隆，将 `--sdk-dir` 指向现有 SDK。

如需构建 HM Pro，在应用仓库根目录执行以下命令，继续使用同一个 `--sdk-dir`。

```powershell
python tools/setup_sdk.py --product hm-pro --sdk-dir ../CAT1.bis_OpenCPU-sdk
python tools/setup_sdk.py --product hm-pro --sdk-dir ../CAT1.bis_OpenCPU-sdk --check
```

后续修改源码时，在应用仓库根目录用对应的准备命令重新同步，再进入 SDK 目录编译。使用自定义路径时，各产品命令的 `--sdk-dir` 均指向该路径。

准备命令附带 `--check` 会校验所选产品的应用副本和构建依赖。准备与编译是两个步骤，随后执行下方对应的构建命令。

新增或删除文件先用 `git add` 暂存，再重新同步。准备脚本同步当前应用仓库的工作区，检测到 SDK 目录中有冲突的手工修改时会停止。三款产品的 `build.bat` 在 SDK 的 `examples/NT26FxDx_OpenKit/firmware` 下生成各自的发布包。HM Pro 单独运行 `build_app.bat` 时，原始应用包位于 SDK 的 `gccout/tirtc_pro_f9d_d_170`。

文档与发布固件请在 `xiaotai-lierda` 应用仓库中查看；SDK 目录用于编译，其根 README 保留原厂说明。

SDK 自带 Windows 编译工具和 GCC 压缩包，首次编译自动解压到 `tools/toolchain/gcc`，无需另外安装 ARM 编译器。pro 的 Python 环境说明见 [Python 环境准备](examples/NT26FxDx_OpenKit/TiRTC-EC718PM-Pro/docs/当前设备操作.md#python-setup)。在准备好的 **SDK 根目录**按产品执行：

```powershell
cd ../CAT1.bis_OpenCPU-sdk
```

| 产品 | 编译命令 | 固件 |
| --- | --- | --- |
| TiRTC-EC718PM-Lite | `.\examples\NT26FxDx_OpenKit\TiRTC-EC718PM-Lite\build.bat` | [下载 lite 最新固件](examples/NT26FxDx_OpenKit/firmware/lite/) |
| TiRTC-EC718PM-Pro | `.\examples\NT26FxDx_OpenKit\TiRTC-EC718PM-Pro\build.bat` | [下载 PM Pro 语音固件](examples/NT26FxDx_OpenKit/firmware/pro/) · [构建说明](examples/NT26FxDx_OpenKit/TiRTC-EC718PM-Pro/docs/构建与固件.md) |
| TiRTC-EC718HM-Pro | `.\examples\NT26FxDx_OpenKit\TiRTC-EC718HM-Pro\build.bat` | [下载 HM Pro 固件](examples/NT26FxDx_OpenKit/firmware/hm-pro/) · [构建说明](examples/NT26FxDx_OpenKit/TiRTC-EC718HM-Pro/docs/构建与固件.md) |

从旧版应用目录升级时，重新运行 `setup_sdk.py` 即可迁移已准备的 SDK。脚本在 SDK 中为旧目录的批处理编译命令保留转发入口，并继续使用 `APP_VARIANT=lite` 或 `APP_VARIANT=pro`。收藏的源码页面可从本页产品入口重新打开。

使用官方原版工具时，EC718PM Pro 首次安装按顺序烧录两个配套包：**先烧录 `_pro_assets.binpkg` 资源安装包，运行并确认 `INSTALL_OK`，再烧录 `_pro_app.binpkg` 正式应用包。** 两次均选择“自定义镜像 → 全量下载”；烧录前备份绑定、设置和需要保留的文件。从源码构建这两个包时，资源安装器和正式应用分别输出到 SDK 的 `gccout/tirtc_pro_assets`、`gccout/tirtc_pro`；`TiRTC-EC718PM-Pro/build.bat` 则发布包含外置资源的 `_pro.binpkg` 合并包。包的选择、成功日志及故障处理见 [pro 两次烧录说明（含历史包示例）](examples/NT26FxDx_OpenKit/firmware/pro/README.md)，板型要求见 [pro 入门指南](examples/NT26FxDx_OpenKit/TiRTC-EC718PM-Pro/docs/当前设备操作.md#build-flash)。

固件按对应应用或资源安装程序的编译完成时间（UTC+8）命名，分别提供 [lite 校验清单](examples/NT26FxDx_OpenKit/firmware/lite/SHA256SUMS.txt) / [pro 校验清单](examples/NT26FxDx_OpenKit/firmware/pro/SHA256SUMS.txt) / [HM Pro 校验清单](examples/NT26FxDx_OpenKit/firmware/hm-pro/SHA256SUMS.txt)。EC718PM Pro 的 `_pro.binpkg` 包含外置 UI 资源。操作见 [pro 多人对讲](examples/NT26FxDx_OpenKit/TiRTC-EC718PM-Pro/docs/多人对讲.md)，包内容和构建方式见 [pro 构建与固件](examples/NT26FxDx_OpenKit/TiRTC-EC718PM-Pro/docs/构建与固件.md)。

HM Pro 固件命名为 `YYYY_MM_DD_HH_MM_SS_hm_pro.binpkg`，同目录的 `SHA256SUMS.txt` 对应发布包。使用原厂工具前先检查目标设备的底包和分区。 [HM Pro 固件与烧录](examples/NT26FxDx_OpenKit/firmware/hm-pro/README.md)

自行编译的合并包使用本次生成的 `SHA256SUMS.txt` 校验。独立应用包和资源安装包可用 `Get-FileHash -Algorithm SHA256` 记录摘要；应用仓库中的清单对应其预编译文件。打包元数据和编译路径可能使新固件的整包哈希与历史发布包不同。

## 从使用到开发

三款产品在同一份 SDK 下各有应用代码、板级配置和编译输出。

| 产品 | 模组与底包 | OpenKit 构建依赖 | 代码架构 |
| --- | --- | --- | --- |
| EC718PM Lite | NT26F6D0 / `F6D_D` | 1.7.0（`Release_V0107`） | [PM Lite](examples/NT26FxDx_OpenKit/TiRTC-EC718PM-Lite/docs/代码与流程详解.md) |
| EC718PM Pro | NT26F6D0 / `F6D_D` | 1.7.0（`Release_V0107`） | [PM Pro](examples/NT26FxDx_OpenKit/TiRTC-EC718PM-Pro/docs/代码与流程详解.md) |
| EC718HM Pro | NT26F9D0 / `F9D_D` | 1.7.0（`Release_V0107`），TiRTC 2.5.0 mini | [HM Pro](examples/NT26FxDx_OpenKit/TiRTC-EC718HM-Pro/docs/代码与流程详解.md) |

## 官方资料

- [TiRTC 产品与接入文档](https://docs.tange.ai/products/tirtc/)
- [小钛体验平台](https://xiaotai.chat/)
- [利尔达快速上手](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/general/quick_run_demo.html)
- [利尔达完整烧录教程](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/tools/flash/flash-tool.html)
- [利尔达文档首页](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/)
- 官方 [SDK 英文说明](https://github.com/lierda-iot/CAT1.bis_OpenCPU/blob/59f1250c4b8caad4bbf4f74167b7b39c79097f8c/README.md) / [SDK 中文说明](https://github.com/lierda-iot/CAT1.bis_OpenCPU/blob/59f1250c4b8caad4bbf4f74167b7b39c79097f8c/README_ZH.md)

探鸽自有的应用代码与文档采用 MIT 许可证。利尔达 OpenKit SDK、原厂派生代码、TiRTC SDK、第三方组件及字体资源保留各自授权。 [许可与第三方说明](THIRD_PARTY_NOTICES.md)
