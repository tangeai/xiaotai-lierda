# 许可与第三方说明

本仓库是基于利尔达 OpenCPU SDK 的 TiRTC 双产品示例。根目录 [LICENSE](LICENSE) 中的 **MIT** 许可适用于探鸽自有的应用代码、构建适配和文档；不改变原厂 SDK、派生代码、第三方组件、字体或预编译库的授权。

## 应用许可

- [lite 应用](examples/L_CT4IT00_YP00W_01_V04/tirtc_lite/LICENSE)：探鸽独立实现及探鸽公开 MIT 参考代码的本地修改统一采用 MIT；利尔达派生文件除外。
- [pro 应用](examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/LICENSE)：探鸽自有的触屏、AI、通话、实时查看及媒体业务采用 MIT；下表中的依赖和派生文件除外。
- 文件中的第三方版权、来源和单独许可证优先适用；不得删除这些通知，也不得将“可随工程分发”解释为所有代码和二进制都受 MIT 许可。

## 保留的上游许可

| 范围 | 许可与阅读入口 |
| --- | --- |
| 利尔达公共 SDK、原厂 Demo 与工具链（`components/`、`config/`、`rules/`、`tools/` 及应用以外的原厂文件） | 利尔达原有开源代码继续适用 Apache-2.0；其中第三方库、工具及供应二进制按自身条款。原仓库 `LICENSE` 已原样保存为 [利尔达 Apache-2.0 许可](LICENSES/Lierda-OpenCPU-Apache-2.0.txt)，包括原版权归属。原始 [SDK 说明](README_SDK.md) / [中文说明](README_ZH.md) 保留原文。 |
| lite 的利尔达派生文件 | `src/app/app_entry.c`、`src/ui/key_input.c`、`include/key_input.h`、`src/ui/ssd1306_display.c`、`include/ssd1306_display.h`、`board/iodriver.ini` 保持 Apache-2.0，见 [来源记录](examples/L_CT4IT00_YP00W_01_V04/tirtc_lite/SOURCE_PROVENANCE.md)。 |
| pro 板级依赖及原厂显示适配 | `board_compat/` 中的原厂文件保留各自许可；`port/tirtc_port.c` 源自利尔达 Demo，继续适用 Apache-2.0。见 [板级依赖](examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/board_compat/README.md)。 |
| 两套 TiRTC SDK 头文件和 `.a` 库 | 仍按独立 TiRTC SDK 协议使用和分发，不属于应用 MIT 范围。见 [lite SDK](examples/L_CT4IT00_YP00W_01_V04/tirtc_lite/sdk/README.md) / [pro SDK](examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/sdk/README.md)。 |
| lite 的 cJSON、Opus、Mbed TLS 等依赖 | 保留 MIT、BSD、Apache 等各自条款，见 [lite 第三方清单](examples/L_CT4IT00_YP00W_01_V04/tirtc_lite/THIRD_PARTY_NOTICES.md)。 |
| pro littlefs、UI 参考代码及公共 SDK 内的 LVGL、二维码等组件 | 保留各自版权和许可证。见 [littlefs BSD-3-Clause](examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/storage/littlefs/LICENSE.md)、[UI 上游 MIT](examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/ui/LICENSE)；公共组件以组件目录和文件头为准。 |
| 字体及由其生成的字库 | 软件 MIT 许可不替代字体许可；原字体和生成资源保留来源及授权边界，见 [字体来源说明](examples/L_CT4IT00_YP00W_01_V04/tirtc_pro/ui/tools/font-sources/SOURCE.md)。 |
| 上游许可副本、历史通知、名称与标志 | `LICENSES/`、`UPSTREAM_LICENSE`、`UPSTREAM_THIRD_PARTY_NOTICES.md` 保留上游原文；历史通知中的 Apache 声明对应其记录的旧版本。名称、标志与商标归各自权利人。 |

## 本次变更记录

2026-09-28，按项目维护者的明确要求，将探鸽自有应用的 Apache-2.0 授权及其在 `MIT AND Apache-2.0` 文件中的自有修改统一为 MIT。核对基线为本仓库提交 `b6c85ce`，许可调整范围可在该提交之后的 Git 差异中查看；探鸽、利尔达和第三方的原版权归属均予保留。

本次调整不改变原厂或第三方授权，不修改业务逻辑、SDK 二进制或已发布的固件。历史 Apache 许可副本继续保留，以便核对上游版本和原厂派生文件。
