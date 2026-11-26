# TiRTC SDK 依赖

TiRTC-EC718HM-Pro 使用 `tirtc__eigencomm-ec71x__gcc-arm-none-eabi-10-2020-q4-major__v2.5.0__mini`。OpenKit 的 `F9D_D` 底包由 `Release_V0107` 自带。准备脚本从 TiRTC 官方软件仓库下载固定配套包，校验归档和构建文件哈希，将构建所需的五个公共头及 `libTiRTC.a` 放在共用 OpenKit SDK 目录中供 HM Pro 使用。 [官方 C SDK 下载页](https://docs.tange.ai/products/tirtc/download.html) · [配套 2.5.0 mini 包](https://repo-sdk.tange-ai.com/repository/tirtc-sdks/releases/eigencomm-ec71x/tirtc__eigencomm-ec71x__gcc-arm-none-eabi-10-2020-q4-major__v2.5.0__mini/tirtc__eigencomm-ec71x__gcc-arm-none-eabi-10-2020-q4-major__v2.5.0__mini.tgz)

已有相同版本的本地 SDK 时，可在准备命令中追加 `--tirtc-sdk-dir <解压目录>`。保留供应包内的 README 与构建信息，目录应包含 `include/tirtc/` 和 `lib/libTiRTC.a`。准备脚本对本地文件使用同一组 SHA-256 校验。 [准备与构建命令](../docs/构建与固件.md)

TiRTC SDK 和应用源码分别适用各自的条款。精确包版本见 [来源记录](../runtime/SOURCE_PROVENANCE.md)，历史资料见 [上游原始通知](UPSTREAM_THIRD_PARTY_NOTICES.md)。
