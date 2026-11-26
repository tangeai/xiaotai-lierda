# 许可与第三方说明

探鸽自有应用、构建适配和文档采用本目录的 [MIT 许可](LICENSE)。仓库根 [LICENSE](../../../LICENSE) 保留利尔达 SDK 的 Apache-2.0 许可原文。原厂代码、第三方依赖和资源适用各自的版权与许可通知。

| 范围 | 来源与许可 |
| --- | --- |
| 利尔达 OpenKit SDK 和板级派生代码 | 依原厂条款使用。`board_compat/` 和 `port/tirtc_port.c` 保留原厂来源及许可 |
| TiRTC SDK | 由匹配的外部 SDK 提供，遵循 TiRTC SDK 条款。 [SDK 说明](sdk/README.md) |
| littlefs、Opus、LVGL 和二维码组件 | littlefs 的 [BSD-3-Clause](storage/littlefs/LICENSE.md) 随应用保留。其余组件的许可随外部 SDK 提供 |
| UI 上游和字体 | [UI 上游许可](ui/LICENSE) · [字体来源和授权](ui/tools/font-sources/SOURCE.md) |
| 本地“我在”提示音 | 使用 Windows SAPI 的 Microsoft Huihui Desktop 和 Microsoft Kangkang 离线合成，固件内置 PCM。生成来源、格式和摘要见[语音资源清单](media/local_ack_pcm.metadata.json) |
| 原始上游说明 | [TiRTC 历史通知](sdk/UPSTREAM_THIRD_PARTY_NOTICES.md) · [运行时许可副本](runtime/LICENSES) |
