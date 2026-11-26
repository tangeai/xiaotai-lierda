# 音频设备和编解码

AI、微信电话、设备电话、实时查看和房间共用 ES8311 录放硬件。引脚和供电由板级配置和显示端口管理。模块接口定义在 [audio_device.h](audio_device.h)，板级入口见 [F9D_D 配置](../boards/f9d_d/README.md)。

## 媒体格式

| 业务 | 媒体格式 |
| --- | --- |
| 本机采播 | 16 kHz、PCM16 单声道。20 ms 为 320 样本、640 字节 |
| AI 对话 | 16 kHz 单声道 Opus。上行以 20 ms 编码，目标码率 24 kbit/s。下行保留完整 Opus 包再分块播放 |
| 设备互呼 | 上行 16 kHz 单声道 Opus。对端需支持 Opus 解码。下行可接收 16 kHz Opus 或 8/16 kHz G.711 A-law |
| 微信、实时查看、房间 | 8 kHz 单声道 G.711 A-law，由适配层与本机 PCM 互转 |

房间上行把相邻两个 16 kHz PCM 样本平均为一个 8 kHz 样本，每 20 ms 编码 160 字节。下行将 A-law 样本解码，并转换至本机采样率。

## 采播生命周期

1. 业务用 owner 和 generation 申请音频租约，再由 `prepare_session()` 初始化或复用 codec、提交音量和麦克风增益。
2. 唯一采集者和唯一播放生产者通过有界缓冲收发 PCM。业务层负责网络编解码、抖动缓冲和播放节奏。
3. 停止时先关闭新输入，等待进行中的调用，再停止和释放租约。SDK 返回 BUSY 时，保留在途资源并按原流程重试。

HM Pro 构建要求 `TIRTC_AUDIO_AEC_EN=y`，使用 `F9D_D` 的 3A 后端连续采播。播放队列暂满时，保留未接受的整块 PCM 再重试。出声、响度与噪声需在目标硬件上验证。 [板级音频配置](../boards/f9d_d/audio.mk)

## 音量及静音

扬声器和麦克风默认都是 8/10，并优先使用已保存的设置。切换开关会静音或恢复生效档位。设备互呼和房间使用单位软件增益并由 codec 应用用户音量，麦克风的 L0 档为静音。这些设置的持久化见 [音频偏好](../preferences/README.md)。

## 来源与许可

`audio_device` 和 `g711_codec` 沿用探鸽利尔达参考实现，保留版权通知。[上游参考](https://github.com/tangeai/xiaotai-lierda/tree/f159bfd3e82cc514dfe6b5b2269b2c96fa8e8f93/examples/L_CT4IT00_YP00W_01_V04/tirtc) · [模块许可](LICENSE)

`opus_codec.c` 和 `opus_codec.h` 复用本仓库 Lite 的 MIT 适配，调用外部 OpenCPU SDK 的 Opus 源码。Opus 及原厂音频 SDK 适用各自的许可。[Lite Opus 适配](../../TiRTC-EC718PM-Lite/src/media/opus_codec.c) · [设备语音参考版本](https://github.com/tangeai/xiaotai-lierda/commit/3ff19701f72f69f8d0e57323670880dda7bae8de) · [第三方许可](../THIRD_PARTY_NOTICES.md)
