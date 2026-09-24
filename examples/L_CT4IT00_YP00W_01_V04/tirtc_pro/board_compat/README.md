# pro 的板级兼容依赖

此目录是当前 pro 应用的构建依赖，不是备份或诊断代码。
目的：两个产品共用公共 SDK 源码，pro 保留已验证的功能和底包。下文历史审核中的 V4 指这套 V04 触屏应用，合并后产品目录命名为 `tirtc_pro`。

## 公共基准（2026-09-23 历史记录）

- 本地：`F:/Lierda_Dev/org_0915/CAT1.bis_OpenCPU-main`，SDK 1.6.0。
- 小钛：[xiaotai-lierda](https://github.com/tangeai/xiaotai-lierda/tree/f159bfd3e82cc514dfe6b5b2269b2c96fa8e8f93)，固定提交 `f159bfd3e82cc514dfe6b5b2269b2c96fa8e8f93`。
- 校验范围：`components/`、`config/`、`rules/`、根 `Makefile`、`build.bat`、`examples/Makefile`。
- 上述范围共 4,541 个文件，逐文件内容与本地 `_0915` 一致，无内容修改、缺失或额外文件；残留空目录也已清理。
- 小钛仓库另外提供的 mbedTLS 许可证保存在本目录 `licenses`；其 `APP_PACKAGE_MEMMAP` 打包参数本工程不需要，公共打包 Makefile 已恢复本地原版。
- 全目录复核后，其他官方 Demo 的差异已恢复、缺失文件已补齐。没有引入利尔达后续 SDK 更新。

本节的本机路径和文件数是 2026-09-23 同步审核记录，不是读者需要准备的目录。公共 SDK 已随本工程提供；固定参考提交用于追溯，实际 pro 构建继续使用下方配套底包和哈希检查。合并后的公共打包 Makefile 保留 lite 所需的 `APP_PACKAGE_MEMMAP` 参数；上面的“已恢复本地原版”仅描述迁移前 pro 独立工程。后续变更不包含在此历史统计中。

## 为什么仍要保留这个目录

当前 V4 已验证版本使用的底包是 1.6.04_beta，和公共 1.6.0 并不是同一套二进制。
音频配置、MQTT 结构、摄像头 DMA、RTOS 兼容和网络符号地址都依赖确切底包。
直接用旧 `.a` 覆盖，会让接口与实际运行代码不匹配；因此把这套依赖移入 V4，原样保留。
`basePkg/F6D_A` 共 53 个文件约 65.26 MiB，完整保存配套底包，避免新旧库混用。

| 内容 | 用途 |
| --- | --- |
| `basePkg/F6D_A` | 整理前已验证的库、AP/CP/bootloader、内存布局和打包信息，字节未改 |
| `include`、`ecapi/PLAT` | 与底包匹配的音频、MQTT、JPEG 和芯片接口，内容未改 |
| `appram.c` | 保留启动清零修正，避免把字节数当作 32 位元素数造成越界 |
| `liot_lcdDev_ST7789.c` | 保留当前屏幕初始化和方向配置 |
| `app.ld` | 保留内存布局、RAM 代码段和资源页面统计符号 |
| `lvgl.mk` | 用公共 LVGL 源码编译，保留有效源文件列表和 Windows 归档路径处理 |
| `board_compat.mk` | 仅在 V4 构建中选择上述依赖，保留原链接顺序 |

公共 SDK 的源文件保持原状；V4 的 Makefile 为启动和 ST7789 两个目标文件选择本目录实现。
匹配的头文件优先加入 V4 包含路径，底包目录由 V4 单独指定，TiRTC 仍是 `sdk` 下的 2.5.0。
`tirtc_app.mk` 和 `runtime/runtime.mk` 的底包哈希检查仍有效，没有放宽或删除检查。

## 验证（2026-09-23）

- V4 正常应用全量 ARM 编译、链接、打包通过。
- 外置资源安装程序编译、链接、打包通过。
- 315 个编译目标文件与整理前逐字节一致。
- 最终 `.elf` 与 `.bin` 均逐字节一致；打包文件仅偏移 12、13 的两个包头字节不同，应用和底包有效载荷相同。
- APP：1,045,776 字节；静态 RAM 范围：488,244 字节；分区不变。
- APP SHA-256：`3cb724c701d31f6e3c288136c1fe0219fceb9ff23ae9fb4a5442eb51f2ba2d5f`。
- ELF SHA-256：`3a58aafa46d0781a95429024a657b6a4df254f530dff615bdb42958676fa234e`。
- 本次未烧录设备，也未重新进行实机通话测试。

## 当前 lite / pro 构建

对外使用根目录 `build_tirtc.bat pro`，生成含外置资源的出厂初始化包 `firmware/pro/latest.binpkg`。其应用构建仍由 `build_tirtc_pro.bat` / 板级 Makefile 选择 `APP_VARIANT=pro`，中间产物在 `gccout/tirtc_pro`。pro 的配置是 `tirtc_pro/config` 文件，业务、`board_compat`、`sdk` 和 `runtime` 均保留在自己的目录。

两产品共用公共 SDK 源码与工具链，不互相引用业务，不复用编译对象。lite 保留公共 1.6.0 底包；pro 继续选择本目录的 1.6.04_beta 底包及匹配头文件。若以后统一底包版本，应另做接口兼容审核和实机回归；不要通过删除哈希检查或替换库来强行通过编译。

上面“验证（2026-09-23）”是合并前的记录；合并后的构建与实机结果以对应发布记录为准。完整说明见 [双产品构建与依赖](../docs/README.md#two-products)。
