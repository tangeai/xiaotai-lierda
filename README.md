# TiRTC × 利尔达 OpenKit

探鸽小钛是面向带屏和无屏设备的开源 AI 音视频固件，让你的硬件支持 **AI 聊天、微信通话、设备对讲和远程实时音视频**。

![探鸽小钛功能示意（具体能力以板型与固件为准）](examples/NT26FxDx_OpenKit/docs/assets/xiaotai.gif)

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

也可以直接下载最新固件，按对应入门页完成烧录。需要从源码编译时，先安装 Git 和 Python 3.10 或更高版本，在本地磁盘的短英文路径（例如 `C:\dev`，避免空格和中文）打开 Windows PowerShell。前四步在同一个父目录执行，第五步再进入 SDK：

```powershell
# 1. 下载利尔达官方 SDK、底包和工具链（固定 1.7.0 版本）
git clone --depth 1 --branch Release_V0107 -c core.autocrlf=false -c core.longpaths=true https://gitee.com/lierda-iot/CAT1.bis_OpenCPU.git CAT1.bis_OpenCPU-sdk

# 2. 下载小钛应用代码
git clone --depth 1 -c core.longpaths=true https://github.com/tangeai/xiaotai-lierda.git xiaotai-lierda

# 3. 自动合入应用并校验 SDK 版本
python xiaotai-lierda/tools/setup_sdk.py --sdk-dir CAT1.bis_OpenCPU-sdk

# 4. 逐文件检查合并结果和构建补丁
python xiaotai-lierda/tools/setup_sdk.py --sdk-dir CAT1.bis_OpenCPU-sdk --check

# 5. 进入 SDK 目录，执行下方 Lite / Pro 编译命令
cd CAT1.bis_OpenCPU-sdk
```

第一条命令先从利尔达官方 Gitee 下载 `Release_V0107`（SDK 1.7.0），其中已包含 `components/basePkg/F6D_D` 底包和编译工具，无需另找底包。第二条命令下载本仓库应用；第三条命令校验官方 SDK 固定提交 `59f1250c4b8caad4bbf4f74167b7b39c79097f8c`，应用必要构建适配，并将应用自动放入 SDK 的 `examples/NT26FxDx_OpenKit` 对应目录。两个仓库目录并列，本仓库只保存小钛应用、文档、固件与构建适配。官方 Gitee 与 [GitHub](https://github.com/lierda-iot/CAT1.bis_OpenCPU) 的该发布标签对应同一提交；也可将第一条的仓库地址替换为 GitHub 地址，保持标签和其他参数不变。若已有该版本的官方 SDK，可跳过第一条命令，将第三条的 `--sdk-dir` 改为现有 SDK 路径。

后续修改源码时，先进入 `xiaotai-lierda` 应用仓库根目录，运行 `python tools/setup_sdk.py` 重新同步，再进入同级 `CAT1.bis_OpenCPU-sdk` 编译；使用自定义 SDK 路径时，每次附带相同的 `--sdk-dir`。

在应用仓库执行 `python tools/setup_sdk.py --check` 可逐文件检查应用副本、构建补丁结果、固定 SDK 提交及已跟踪 SDK 文件的本地改动；发布固件不参与这项检查。编译仍需单独执行下方产品命令，产品构建规则会检查底包及依赖哈希。

新增或删除文件先用 `git add` 暂存，再重新同步。准备脚本只复制本地当前工作区内容，不执行 `commit` 或 `push`；全新克隆获取的是已提交版本。SDK 目录中的手工修改会受到保护，出现冲突时停止合并。编译生成的固件位于 SDK 的 `examples/NT26FxDx_OpenKit/firmware`，不会覆盖本仓库已有的发布固件。

文档与预发布固件请在 `xiaotai-lierda` 应用仓库中查看；SDK 目录用于编译，其根 README 保留原厂说明。

SDK 自带 Windows 编译工具和 GCC 压缩包，首次编译自动解压到 `tools/toolchain/gcc`，无需另外安装 ARM 编译器。pro 的 Python 环境说明见 [Python 环境准备](examples/NT26FxDx_OpenKit/tirtc_pro/docs/当前设备操作.md#python-setup)。在准备好的 **SDK 根目录**按产品执行：

| 产品 | 编译命令 | 最新固件 |
| --- | --- | --- |
| lite | `.\examples\NT26FxDx_OpenKit\tirtc_lite\build.bat` | [下载 lite 最新固件](examples/NT26FxDx_OpenKit/firmware/lite/) |
| pro | `.\examples\NT26FxDx_OpenKit\tirtc_pro\build.bat` | [下载 pro 最新固件](examples/NT26FxDx_OpenKit/firmware/pro/) |

下载应用仓库中的预发布固件、使用官方原版工具时，pro 推荐按顺序烧录两个配套包：**先烧录 `_pro_assets.binpkg` 资源安装包，运行并确认 `INSTALL_OK`，再烧录 `_pro_app.binpkg` 正式应用包。** 两次均选择“自定义镜像 → 全量下载”；烧录前备份绑定、设置和需要保留的文件。从源码构建这两个包时，资源安装器和正式应用分别输出到 SDK 的 `gccout/tirtc_pro_assets`、`gccout/tirtc_pro`；`pro/build.bat` 则发布包含外置资源的 `_pro.binpkg` 合并包。包的选择、成功日志及故障处理见 [pro 两次烧录说明](examples/NT26FxDx_OpenKit/firmware/pro/README.md)，板型要求见 [pro 入门指南](examples/NT26FxDx_OpenKit/tirtc_pro/docs/当前设备操作.md#build-flash)。

固件按对应应用或资源安装程序的编译完成时间（UTC+8）命名，分别提供 [lite 校验清单](examples/NT26FxDx_OpenKit/firmware/lite/SHA256SUMS.txt) / [pro 校验清单](examples/NT26FxDx_OpenKit/firmware/pro/SHA256SUMS.txt)。pro 的 `_pro.binpkg` 是包含外置资源镜像的合并包，只有已验证支持本板外置 Flash 写入的工具才能使用，官方原版工具请按上述两次烧录流程选择文件。操作见 [pro 多人对讲](examples/NT26FxDx_OpenKit/tirtc_pro/docs/多人对讲.md)，包内容和构建方式见 [pro 构建与固件](examples/NT26FxDx_OpenKit/tirtc_pro/docs/构建与固件.md)。

自行编译的包使用 SDK 输出目录中本次生成的 `SHA256SUMS.txt` 校验；应用仓库中的清单只校验其预发布文件。打包元数据和编译路径可能使新固件的整包哈希与历史发布包不同。

## 从使用到开发

两套产品共用公共 SDK 源码和工具链，应用代码、配置与编译输出各自独立。当前使用官方 SDK 1.7.0 和 `F6D_D` 底包，默认开启利尔达 AEC；AI 对话支持边播边听与插话打断。按产品入门页的“操作 → 架构 → TiRTC 接入 → AI 呼叫配置”继续阅读：[lite 代码架构](examples/NT26FxDx_OpenKit/tirtc_lite/docs/代码与流程详解.md) / [pro 代码架构](examples/NT26FxDx_OpenKit/tirtc_pro/docs/代码与流程详解.md)。

## 官方资料

- [TiRTC 产品与接入文档](https://docs.tange.ai/products/tirtc/)
- [小钛体验平台](https://xiaotai.chat/)
- [利尔达快速上手](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/general/quick_run_demo.html)
- [利尔达完整烧录教程](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/tools/flash/flash-tool.html)
- [利尔达文档首页](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/)
- 官方 [SDK 英文说明](https://github.com/lierda-iot/CAT1.bis_OpenCPU/blob/59f1250c4b8caad4bbf4f74167b7b39c79097f8c/README.md) / [SDK 中文说明](https://github.com/lierda-iot/CAT1.bis_OpenCPU/blob/59f1250c4b8caad4bbf4f74167b7b39c79097f8c/README_ZH.md)

探鸽自有的应用代码与文档采用 MIT 许可证（[lite](examples/NT26FxDx_OpenKit/tirtc_lite/LICENSE) / [pro](examples/NT26FxDx_OpenKit/tirtc_pro/LICENSE)）。利尔达 OpenKit SDK、原厂派生代码、TiRTC 预编译 SDK、第三方组件及字体资源保留各自授权，具体范围见 [许可与第三方说明](THIRD_PARTY_NOTICES.md)。
