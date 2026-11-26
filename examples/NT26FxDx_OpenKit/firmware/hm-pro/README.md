# HM Pro 固件下载与烧录

[HM Pro 产品首页](../../TiRTC-EC718HM-Pro/README.md) · [构建与固件](../../TiRTC-EC718HM-Pro/docs/构建与固件.md) · [设备操作](../../TiRTC-EC718HM-Pro/docs/当前设备操作.md)

> 适配 NT26F9D0（EC718HM）OpenKit，使用 `F9D_D` 1.7.0 底包。完整固件包含配套底包、应用和内置 UI 资源，可用一个 `.binpkg` 文件安装。

<a id="download"></a>

## 1. 下载与校验

当前正式版为 [2026_11_26_19_23_29_hm_pro.binpkg](2026_11_26_19_23_29_hm_pro.binpkg)。在文件预览页下载原始文件，核对 [SHA-256 校验清单](SHA256SUMS.txt)。文件名时间取应用编译完成时间（UTC+8）。

在 Windows PowerShell 中进入下载目录后计算摘要。

```powershell
Get-FileHash -Algorithm SHA256 .\2026_11_26_19_23_29_hm_pro.binpkg
```

烧录前将结果和清单中同名条目的十六进制摘要核对，字母大小写等价。摘要不一致时重新下载对应的完整包。

<a id="flash"></a>

## 2. 烧录固件

打开 [HM Pro 网页烧录](https://xiaotai.chat/web-flasher/?board=lierda-ec718hm-pro)，选择上述完整包，核对板型、底包和写入范围，按下表选择操作。

| 设备状态 | 下载方式 |
| --- | --- |
| 已安装匹配的 `F9D_D` 1.7.0 和原生分区 | 网页备份后仅更新 APP。使用原厂工具时，选择同一完整包并仅写入 APP。 |
| 网页识别为 `F9D_D` 1.7.1 | 从 1.7.1 切换到本包的 1.7.0 前，先导出并保存备份。页面提供重装选项时，确认清除应用数据后再继续。完成后重新绑定设备并恢复设置。 |
| 空白板、未知底包或无法确认分区布局 | 使用利尔达原厂初始化或恢复流程，先确认对应板卡的备份、校准数据保留和分区初始化步骤。 |

1. 安装适用于 NT26F9D0 的驱动和利尔达原厂下载工具。用数据线连接设备，关闭占用设备串口的程序。
2. 在工具中选择“下载固件”，启用“自定义镜像”，浏览并选择已校验的完整包。
3. 按上表选择对应的写入方式和范围，在下载期间保持供电与 USB 连接，等待工具报告成功。
4. 退出下载模式并开机，检查界面、触摸和 4G 注册。迁移后按需恢复设置或重新绑定设备。

工具下载、驱动安装及进入下载模式的方法见原厂手册。 [利尔达原厂烧录教程](https://opendocs.lierda.com/docs/CAT.1_Doc_Protal/zh_CN/tools/flash/flash-tool.html) · [板级分区与 ABI](../../TiRTC-EC718HM-Pro/boards/f9d_d/ABI.md)

<a id="build"></a>

## 3. 从源码重新构建

准备外部 OpenKit SDK 1.7.0 后，从 **SDK 根目录**运行 HM Pro 的正式构建入口。 [SDK 准备与构建命令](../../TiRTC-EC718HM-Pro/docs/构建与固件.md)

```powershell
.\examples\NT26FxDx_OpenKit\TiRTC-EC718HM-Pro\build.bat
```

正式入口将带时间戳的固件和校验清单写入 SDK 的 `examples/NT26FxDx_OpenKit/firmware/hm-pro/`。发布时将本次构建的固件和对应清单复制到应用仓库的同名目录。
