# 共用音频硬件与 G.711 适配

AI、微信电话、设备电话和 LIVE 共用同一 ES8311 录放硬件。派生自 [小钛利尔达参考](https://github.com/tangeai/xiaotai-lierda/tree/f159bfd3e82cc514dfe6b5b2269b2c96fa8e8f93/examples/L_CT4IT00_YP00W_01_V04/tirtc) 的 `audio_device/g711_codec`，保留 Apache-2.0 文件许可头。当前接口见 [audio_device.h](audio_device.h)。

## 硬件和格式

- ES8311：I²C0 / I²S0，PA 为 GPIO11；模块作主时钟，codec 从机，单声道右通道、16-bit word/slot。
- I²S 对应模组 PAD26/30/31/32/33；PA_SD 为 PAD29；I²C0 SCL/SDA 为 PAD39/38。
- 屏幕端口先启用共享 3.3 V 电源。本模块不反复操作 LCD、触摸或 Flash 的公共供电。
- 硬件工作在 **8 kHz 单声道 PCM16**。20 ms 对应 160 样本、320 字节 PCM、160 字节 A-law。
- 一次 SDK Record 获取 40 ms/640 字节，分两次返回 20 ms；第二次使用缓存。LIVE 下行 16 kHz 转 8 kHz 在上层完成，不是 codec 切成 16 kHz。

## 生命周期

1. 业务获得带 owner/generation 的音频租约。
2. `prepare_session()` 初始化或复用 codec，并一次提交有效音量/增益。
3. 单一采集者录音；单一播放生产者提交有界 PCM。业务层管理 A-law 编解码、抖动缓冲和播放节奏。
4. 控制操作与采播在途状态互斥，不能在短临界区内执行阻塞 SDK 操作。
5. 结束时停止新输入，等待正在执行的 SDK 调用，stop，再 release。BUSY 保留租约并等待，不强行 deinit 或释放旧内存。

SDK Record 可能无超时等待硬件；Play 复制数据后异步输出；Stop 不是同步 DMA 完成屏障。FINISH 与队列空只能辅助调度，不能单独作为内存释放依据。

## 开关、音量与首字预热

出厂默认扬声器 8/10、麦克风增益 10/10、两开关开启；已保存配置优先。开关关闭将有效级别静音，但保留用户原数值。硬件映射与 G.711 无关，见源码 `set_levels/apply_levels`。

连接期间 `prewarm_session()` 可先提交 1200 ms 静音 DAC 时钟；首块播放再带剩余 80 ms。未完成提前预热时，首块流程承担完整 1280 ms；后续新段或断流恢复保留 80 ms。它与网络连接尽量重叠，不能直接解释成用户点击后固定等待 1.28 秒。

改变音量、静音、停止、换租约会丢弃缓存 PCM，避免旧声音流入新状态。AI 的播放抑制属于上层策略；本底包适配没有可承诺的 AEC/ANS/AGC 全双工处理，`use3A` 保持关闭。

## 排查时区分三个结果

- 编码/发送接口接受了音频：不证明远端播放完成。
- SDK Play 接受 PCM：不证明 PA/喇叭已经出声。
- FINISH 或估算音频尾部到达：不证明所有 DMA 资源可立即释放。

周期峰值、采集批次、播放计数受调试开关控制，默认只保留启动与必要错误；不输出语音样本。真实声音、增益、供电噪声和声学回声须实机听测。完整业务音频路径见 [代码篇](../docs/代码与流程详解.md)。
