# 运行时来源与版本

应用侧运行时沿用小钛 OpenKit Pro 的 TiRTC 适配，并针对 OpenKit 1.7.0 原生的 NT26F9D0 `F9D_D` 进行 ABI 校验。[许可副本](LICENSES) 与原始通知保留其源代码归属。当前使用的版本和精确哈希如下。

| 依赖 | 构建校验 |
| --- | --- |
| 共用 OpenKit SDK 及原生 `F9D_D` 文件来源 | `59f1250c4b8caad4bbf4f74167b7b39c79097f8c`（`Release_V0107`，1.7.0） |
| TiRTC EC71x 2.5.0 mini 归档 | `9c1cf0ac1f3b0c7f9df25837453c079935997fc28bf576c6531e3b193fbc8e65` |
| TiRTC EC71x 2.5.0 mini 库 | `31c72553a082104b28eb8a2baa3e837b87afd25a54c2330eadc640dd0f287a5d` |
| TiRTC 公共头文件 | SDK 准备脚本验证配套文件摘要 |

[TiRTC SDK 依赖](../sdk/README.md) · [准备 SDK 与构建应用](../docs/构建与固件.md)
