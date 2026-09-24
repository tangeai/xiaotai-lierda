# 运行时来源与适配证据

- 上游：https://github.com/tangeai/xiaotai-lierda
- commit：`f159bfd3e82cc514dfe6b5b2269b2c96fa8e8f93`（2026-09-15）。源路径为 `examples/L_CT4IT00_YP00W_01_V04/tirtc/src/tirtc_runtime.c` 及 `include/tirtc_runtime.h`。
- 原通知与 `MIT AND Apache-2.0` 标识保留；完整文本在本目录 `LICENSES/`。
- 2026-09-23 按用户提供的 `tirtc__eigencomm-ec71x__gcc-arm-none-eabi-10-2020-q4-major__v2.5.0__mini` 原样更新库和配套头文件，SHA-256：`31c72553a082104b28eb8a2baa3e837b87afd25a54c2330eadc640dd0f287a5d`。内部版本为 `v2.5.0-9088239c` / `f72f5d3c`；此前工程实际为 `v2.5.0-87c3c290`，并非 2.3。5 个公共头文件字节相同。
- 保留的本地 F6D_A `ap_lierda_app.elf` SHA-256：`8bc2f616e021fb1c98f36afcf2a7f09d0de02937e6cf00909c202b920b0301e0`。这与最新上游底包不同，不能直接沿用上游底包哈希或假定所有 ABI 相同。

## 本地适配

1. 单任务启动入口，保留成功创建的常驻 semaphore / queue，失败允许重试；避开当前 F6D_A 删除空 semaphore 会先永久等待的问题。
2. 复用现有绑定身份和网络快照，以 SIM、route generation、binding generation 及独立的非零 credential_epoch 防止旧连接/回调跨身份继续可用；同身份同路由的重新绑定也不能沿用旧媒体会话。
3. 保留上游句柄引用、延迟断连、错误回调和待完成连接上下文管理，不在 SDK 回调里销毁 SDK。
4. 原生库与当前工具链链接选用 `-fno-lto`；精确哈希变化时 Makefile 停止编译，要求重新审查。
5. 保留 `Logf`、`printf`、`app_log_time` 安全日志包装；凭据类原生日志被过滤，受控日志不打印 token、签名或完整身份。
6. 保留 `tgtrp_connection_set_on_error` ABI 适配以及 `freertos_ThreadCreateWithStackSize` 栈包装。后者并不替代本工程独立的 Liot task_create wrapper。
7. `sha256_hmac` 用当前 mbedTLS 头文件语义选择 SHA256。固定 SDK 的原生调用传枚举 9，而本地底包 `mbedtls_md_info_from_type` 仅接受 3..8；本地 SHA256 是 6，直接调用会返回空。
8. `strdup/strndup` 使用兼容本项目 RTOS 堆的分配路径，不让 native SDK 字符串复制落入未配置的 newlib sbrk 堆。

本次新库的 native 平台、加密、日志和 RTC 线程对象的可加载节与之前相同。
`TiRtcConnect`、`_tirtc_create_conn`、`_on_error`、`TiRtcDisconnect`、
`TiRtcWhipConnect/Accept`、`TiRtcSendAudio/VideoStream` 的反汇编及调用目标相同，
因此保留既有适配，仅将设备呼叫失败清理的版本门禁扩展到上述新构建。
历史二进制对照产生了原件、哈希清单与反汇编证据；这些诊断产物不作为当前开源应用的构建依赖随包提供。保留的上游原文中的旧哈希不代表当前库，当前库身份以上述记录和构建门禁为准。

## 历史审查的证据类型


- `sha256_hmac_disasm.txt`：SDK `sha256_hmac+0xa` 的枚举常量 9。
- `mbedtls_md_info_from_type_base_disasm.txt`：当前底包枚举范围与分派。
- `freertos_ThreadCreateWithStackSize_disasm.txt`：原生 shim 的请求右移及线程参数。
- `xTaskCreate_base_disasm.txt`：底包 `0x7f62` 将栈深度左移 2，实际按 4 字节 word 分配。
- `xQueueGenericCreateStatic_base_disasm.txt`：当前静态队列构造路径，marker 在控制块 +70。
- `sdk_measurement.json` / `sdk_audio_wrapped_size.txt`：启用 6 个 wrapper 后的音频符号闭包测量及完整链接参数。

以上为历史审查输出的名称，不是当前仓库中的下载链接。需要重新审查时，应针对固定版本的实际库重新生成反汇编与测量记录；结论不能推广到其他底包或 SDK 版本。
