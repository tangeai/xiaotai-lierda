# 许可与第三方说明

本产品基于利尔达 OpenCPU SDK。探鸽自有应用、构建适配与文档采用本目录 [MIT 许可](LICENSE)；仓库根 [LICENSE](../../../LICENSE) 保留官方 SDK 的 Apache-2.0 原文。应用 MIT 不改变原厂派生代码、第三方组件、字体及预编译库的授权。

| 范围 | 许可与来源 |
| --- | --- |
| 公共 SDK、原厂 Demo 与工具 | 原厂开源代码保持 Apache-2.0；供应二进制、第三方组件及工具仍按自身条款。见 [SDK 原始说明](../../../README_SDK.md)、[中文说明](../../../README_ZH.md) 与各文件通知。 |
| 探鸽自有 Pro 应用 | 使用 [MIT](LICENSE)，包括触屏、AI、通话、实时查看、应用侧构建与文档；下列依赖和派生文件除外。 |
| 板级适配及原厂显示适配 | `board_compat/` 内原厂派生文件保留各自许可；`port/tirtc_port.c` 源自利尔达 Demo，继续适用 Apache-2.0。见 [板级依赖说明](board_compat/README.md)。 |
| TiRTC SDK 头文件及预编译库 | 按独立 SDK 协议使用和分发，不属于应用 MIT 范围，见 [SDK 分发说明](sdk/README.md)。随工程提供不代表算法源码公开，也不扩大其授权。 |
| 平台与 runtime 适配 | 原版权和固定来源记录保留，见 [平台来源](platform/SOURCE_PROVENANCE.md) 与 [runtime 来源](runtime/SOURCE_PROVENANCE.md)。 |
| littlefs、UI 及公共 SDK 组件 | 保留各自版权和许可证，见 [littlefs BSD-3-Clause](storage/littlefs/LICENSE.md)、[UI 上游 MIT](ui/LICENSE)；LVGL、二维码等公共组件以组件目录和文件头为准。 |
| 字体及生成字库 | 应用 MIT 不替代字体授权，见 [字体来源](ui/tools/font-sources/SOURCE.md)。 |
| 上游许可副本和原始通知 | 产品子目录内 `LICENSES/`、`UPSTREAM_LICENSE` 和 `UPSTREAM_THIRD_PARTY_NOTICES.md` 保留上游原文，其中旧路径及版本描述按原工程解释。名称、标志与商标归各自权利人。 |

公共 SDK 为维护者下载的官方 1.7.0，两产品使用 `F6D_D`；各自 TiRTC 库和第三方依赖仍遵守独立条款。Lite 的许可边界见 [Lite 第三方说明](../tirtc_lite/THIRD_PARTY_NOTICES.md)。

2026-09-28 按项目维护者要求，探鸽自有部分原有 Apache-2.0 授权及 `MIT AND Apache-2.0` 中的自有修改统一为 MIT；原厂和第三方权利不变。许可来源记录与原始版权通知保留，不随构建缓存或旧固件清理删除。
