# pro 的板级兼容依赖

此目录是当前 pro 应用的构建依赖，不是备份或诊断代码。
目的：两个产品共用公共 SDK，pro 只保留产品需要的启动、屏幕和链接适配。下文历史审核中的 V4 指这套 V04 触屏应用，产品目录为 `examples/NT26FxDx_OpenKit/TiRTC-EC718PM-Pro`。

## 当前基线（2026-10-09）

- 公共 SDK 使用利尔达官方 OpenKit `1.7.0`。Lite、Pro 均使用公共 `components/basePkg/F6D_D` 及公共头文件，不再使用 Pro 私有 `1.6.04_beta` 底包。
- 原 `basePkg`、`include`、`ecapi` 和 `lvgl.mk` 已退出当前构建并从发布工程移除；发布工程不需要另行准备旧 SDK。
- `tirtc_app.mk`、`runtime/runtime.mk` 保留精确哈希门禁，并在反汇编和结构布局审计后更新到本次官方 D 基线。底包 ELF SHA-256：`9553cae08079eab350557b86d5474464ffabb3086e7055739c7b57fccc930c3d`。
- Pro 使用 `CODE_REGION_ADJUST=0x4B000`：程序区 `0x2A6000..0x3BC000`（1112 KiB），内部文件系统仍为 `0x3BC000..0x3E4000`（160 KiB）。应用及底包必须成套重新构建，不能混刷 A 的旧应用镜像。
- 底包迁移后，当前 Pro 已同步 Lite 的 3A／AEC 适配，默认 `TIRTC_AUDIO_AEC_EN=y`、延迟参数 `10`。适配在 Pro 自有 `media`、AI 及音频调用处完成，官方 SDK 不改；`TIRTC_AUDIO_AEC_EN=n` 保留普通录放音路径。

| 当前保留内容 | 用途 |
| --- | --- |
| `appram.c` | 保留 ZI 清零边界修正；当前官方启动代码仍存在字节数与 32 位元素数混用问题 |
| `liot_lcdDev_ST7789.c` | 保留产品屏幕初始化和方向配置 |
| `app.ld` | 产品 RAM 段、链接布局和资源页面统计符号；分区容量来自当前 D 配置 |
| `board_compat.mk` | 使用公共官方底包，仅替换产品所需启动和屏幕目标，并保持链接顺序 |
| `licenses` | 保留历史引入依赖的许可证 |

## 兼容审计

当前语音固件的应用构建关闭摄像头驱动和视频链路。下表摄像头与 JPEG 项用于追溯 V4 审计。

| 项目 | 当前处理与依据 |
| --- | --- |
| RTOS task-create | 移除旧 `xTaskResumeAll` 补偿；新库已在创建失败和成功路径恢复调度，旧补偿会重复恢复 |
| RTOS semaphore-delete | 保留常驻对象及安全销毁逻辑；新库仍先无限等待一个 token，再删除 semaphore |
| 摄像头 DMA fence | 历史 V4 审计：CSPI/Camera 反汇编与旧版相同，回调表仍为 20 字节，frame-end 与 DMA-end 仍共用通知，必须另等 DMA 完成 |
| JPEG/video | 历史 V4 审计：两库反汇编及 JPEG 头文件未发生相关契约变化，解码仍写入调用者提供的行缓冲区 |
| Preferences | 保留 `LFS_stat` 原错误码路径；新底包仍经串行任务返回 LittleFS 结果，stat 字段偏移未变；NVM 内部对象尺寸由新库自行管理 |
| 普通 HTTP | 保留已有失败 OPEN、队列重置与 CLOSE 后释放处理；`libliot_https.a` 反汇编与旧版相同，下层 HTTP 库仅日志常量变化 |
| 音频 | 默认 3A 流式录放音，保留 0 档静音，录放音档位曲线对齐 Lite；采用 Lite 的先静音并等待约 200 ms 排空、再停录音和播放、重建采集及音量复位纠正。关闭 AEC 时保留原 Pause/Resume/Stop 及对齐处理 |
| TiRTC runtime | 保留 SHA256 枚举、日志、线程等适配；已核对的 RTOS/lwIP/mbedTLS 类型布局与旧版一致；`netif_list` 由当前 ELF 提取，不能沿用旧地址 |

构建参数和固件用途见 [构建说明](../docs/构建与固件.md)。

历史参考：V4 的 SDK 基线见[对应源码提交](https://github.com/tangeai/xiaotai-lierda/tree/f159bfd3e82cc514dfe6b5b2269b2c96fa8e8f93)，引入依赖的许可证保存在 `licenses`。

## 当前 lite / pro 构建

对外使用 SDK 根目录 `examples\NT26FxDx_OpenKit\TiRTC-EC718PM-Pro\build.bat`，生成含外置资源的出厂初始化包 `examples/NT26FxDx_OpenKit/firmware/pro/YYYY_MM_DD_HH_MM_SS_pro.binpkg`。其应用构建仍由 `examples\NT26FxDx_OpenKit\TiRTC-EC718PM-Pro\build_app.bat` / 板级 Makefile 选择 `APP_VARIANT=pro`，中间产物在 `gccout/tirtc_pro`。pro 的配置是 `TiRTC-EC718PM-Pro/config` 文件，业务、`board_compat`、`sdk` 和 `runtime` 均保留在自己的目录。

两产品共用当前官方 SDK、F6D_D 底包与工具链，不互相引用业务，不复用编译对象。后续升级仍应重新核查接口兼容及实机行为；不要通过删除哈希检查或盲目替换哈希来强行通过编译。

当前构建方式见 [构建与固件](../docs/构建与固件.md)。完整说明见 [双产品构建与依赖](../docs/README.md#two-products)。
