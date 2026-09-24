# 外置 Flash 的 UI 资源

## 默认用法：固件与资源一次烧录

在仓库根目录执行：

```powershell
.\build_tirtc.bat pro
```

生成 [firmware/pro/latest.binpkg](../../../../firmware/pro/latest.binpkg)，其中包含正常固件与配套外置资源文件系统。选择这个出厂初始化包进行一次全量烧录，重启即运行正常应用，不必先烧资源安装程序再烧应用。

**出厂初始化包会覆盖外置 Flash 文件，请先备份需要保留的文件。** 首次使用步骤见 [操作篇](../docs/当前设备操作.md#assets)。下面是资源内容和开发维护方式，不是首次使用必须再做的步骤。

## 资源内容

- `/ui14-font.bin`：631,201 字节，原始 4 bpp 字形。
- `/ui13-bg.bin`：153,600 字节，原始 RGB565 背景。
- `asset_manifest.h` 保存长度和 CRC32，加载时校验。
- 当前应用按字形缓存字体，背景常驻；没有把完整字库复制到 RAM。
  缓存大小以 `font_cache.h` 中 `TIRTC_FONT_CACHE_BYTES` 为准。

## 开发维护：重新提取资源与独立安装器

在工程根目录执行：

```powershell
python examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/resource_store/prepare_external_assets.py
.\build_tirtc_pro_assets.bat
```

生成工具读取当前字库和背景源码，更新 `asset_manifest.h`、`installer_blob.c`
以及 `release/tirtc_pro/assets14`。字库原件和生成工具仍在 `ui/tools`；资源未重新压缩或降低精度。

脚本对当前字库的长度和 SHA-256 有固定检查，以上命令用于重建**这一套已确定的资源**，不是任意换字体后都能直接执行的通用打包器。更换字库或背景时，应同步审核源数据、脚本的长度/哈希条件、manifest 与安装程序，并重新生成和验证应用/资源包；不要只去掉断言继续发布。

独立安装程序作为维护选项保留：它校验已有文件，仅写入缺失或不匹配的上述两项资源，完成后打印
`[assets14] INSTALL_OK`。出现 `INSTALL_FAILED` 时停止，并保留错误信息。
选择此维护模式时，安装完成必须重新烧录正常应用包。维护用的应用包可以保留既有外置文件；默认 `latest.binpkg` 则是覆盖外置文件的出厂初始化包，两者用途不同。

正常应用不包含资源安装入口和完整资源大数组；它从外置文件区读取，
保留文件系统保护、CRC 校验、失败重试和字体缓存逻辑。

因此，出厂包把固件与外置资源一起交给烧录工具写入，并不意味着把完整字库常驻应用 RAM，也不改变正常应用的资源加载逻辑。

### 出厂打包的检查

`build_factory_image.py` 使用仓库自带的 `lfsutil` 生成 4 MiB、4 KiB 块的 LittleFS 2.0 镜像，检查几何信息并逐文件提取、比对资源。`package_factory.py` 使用自带 `fcelf` 合包，检查五项载荷的地址、容量、SHA-256，以及前四项与正常应用包一致后，才更新 `firmware/pro/latest.binpkg` 和校验清单。

中间文件保存在 `gccout/tirtc_pro_factory`。两个工具只在电脑上生成文件，不访问设备；包内容检查不代替实机外置 Flash 烧录验证。

## 与文档配图的关系

正常固件使用外置资源；文档离线渲染使用生成这些资源的同一份字体和背景源码，因此画面布局与素材一致。渲染图不验证 SPI 挂载、CRC 或上电安装；实机仍需确认资源安装成功。

返回 [操作篇资源安装](../docs/当前设备操作.md#assets)。
