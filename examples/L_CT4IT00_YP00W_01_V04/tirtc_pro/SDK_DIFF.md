# SDK 同步历史记录（2026-09-23，45 版）

**当前目录说明：** 本应用现位于 `tirtc_pro`，对外构建使用根目录 `build_tirtc.bat pro`，生成含外置资源的出厂初始化包；与 lite 共用公共 SDK 源码和工具链，保留 pro 专属兼容依赖。本文下方是 2026-09-23 的历史记录，其中旧入口名称、路径和数量不作为合并后用法。

这是当时 SDK 同步后的快照，文件数量、唯一差异和构建目录均限定于那一轮。后续已新增 AI 呼叫兼容逻辑、重写项目首页和文档、加入 `assets/videos` 操作录像，不能把本记录当成当前整个目录“仍只有一个文件不同”的结论。当前包及验证范围见 [发布说明](../../../release/tirtc_pro/README.md)。

比较基准：`F:/Lierda_Dev/org_0915/CAT1.bis_OpenCPU-main`。
本次比较了整个目录的文件内容，不只比较公共 SDK，也不以时间戳推断内容相同。

上述本机路径是维护者的历史比对来源，不是构建所需路径；读者使用本仓库随附的 SDK 和 [板级依赖](board_compat/README.md) 即可，勿创建同名绝对路径或用后续官方 SDK 直接覆盖。

## 当轮最终结果

- 原始工程共 4,861 个文件：4,860 个内容完全一致，0 个缺失。
- 唯一内容不同的原始文件是 `examples/L_CT4IT00_YP00W_01_V04/Makefile`。
  它增加 `BUILD_MODE=tirtc` 分支；原厂 Makefile 的完整内容保留在另一个分支中。
- `components`、`config`、`rules`、原有工具文件、原厂 Demo 内容均与本地原版一致。
- 相同文件同步了原版修改时间，避免仅按时间比较时出现大量假差异。
- 新增的业务、专用配置及必要依赖全部位于本目录 `tirtc`；V4 已验证底包未被降级。

## 截图中差异的来源与处理

| 项目 | 原因及结果 |
| --- | --- |
| `examples/demo`、`examples/router` | 原来有 9 个不同的文件，已恢复本地原版 |
| 缺失的原厂文件 | 47 个文件已补齐，包括 V04 原厂 app/demo/inc 和双眼 Demo 依赖 |
| `examples/NT26FxDx_OpenKit` | 属于本地原版没有的另一套示例，TiRTC 不依赖，已移出工程 |
| 板级 `config` | TiRTC 配置移入 `tirtc/config`，板级配置恢复原厂内容 |
| `components/basePkg/Makefile` | 上轮采用的小钛打包参数与本地原版不同，本轮恢复本地原版 |
| `components/thirdparty/mbedtls/LICENSE` | 小钛额外提供的许可证，移至 `tirtc/board_compat/licenses` 保留 |
| `components/presetfile` 等 | 已经没有文件的残留目录，已清理 |
| `tools/lfsutil` | 工具恢复本地原版；额外 Linux 工具已移出工程 |
| `tools/toolchain/gcc` | 原厂压缩包自动解压生成，6,412 个文件共 728,411,583 字节；非空文件全部通过原压缩包 CRC 校验，无内容修改 |
| `tools/msys64` | 原有文件内容一致；文件修改时间已同步原版 |
| `gccout` | 编译输出，只保留本轮 `tirtc_v04`，旧构建目录已清理 |
| `release` | 交付固件和外置资源，原始工程没有，属于正常新增产物 |
| 根 `build_tirtc_ui.bat`、`README_TIRTC_UI.md` | TiRTC 专用的一键编译入口和说明 |

编译器解压目录和 `gccout` 已经被原厂 `.gitignore` 排除。
删除解压后的编译器不会减少源码差异；下一次执行原厂编译流程还会自动生成。
比较当轮源码时排除了 `tools/toolchain/gcc`、`gccout`、`release`，当时其余差异为应用目录、
一个板级 Makefile 和上述两个项目入口/说明文件。当前文档及演示素材的新增不属于公共 SDK 修改。

## 编译与烧录包

正常编译执行根目录 `build_tirtc_ui.bat`，它传入 `BUILD_MODE=tirtc`。
原厂分支仍可使用 `BUILD_MODE=app` 或 `BUILD_MODE=demo`，不同模式请使用不同 `BUILDDIR`。

本轮全量编译产物：

`release/TiRTC_UI_V04_NT26F6D0_45_sdk2.5.0_sdk_sync_20260923.binpkg`

- 3,366,040 字节；SHA-256：`a3af9552120290469644db50c6c536a3bf43d65e007d376b4bad9b5f6d9065bb`。
- 功能版本仍为 45；该文件名标识本次 SDK 目录同步后的重新编译包。
- 正常 TiRTC 应用、外置资源安装程序、原厂 `BUILD_MODE=app` 分支均编译、链接、打包成功。
- 正常应用 `.bin`、`.elf` 与整理前逐字节一致，原业务代码、底包和内存布局没有变化。
- APP Flash：1,045,776 字节；静态 RAM 范围：488,244 字节。
- 未烧录设备，等待本轮实机验证。已有 UI 资源的设备只需更新正常应用包。

完整应用依赖边界见 [board_compat/README.md](board_compat/README.md)。
