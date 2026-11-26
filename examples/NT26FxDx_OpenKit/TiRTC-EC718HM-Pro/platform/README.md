# 小钛体验平台适配

入口为 `tirtc_platform.h`。启动一个优先级 8、16 KiB 栈的后台任务；主任务每 5 秒调用 `tirtc_platform_publish()`，只复制缓存，不访问模组或服务器。UI 操作仅合并重试请求，不执行网络 I/O。

## 实际流程

1. 等待网络快照的当前 SIM、CID1、IP 与有效 UTC 时间。使用网络 `generation` 识别承载变化；旧代次响应不再作为成功处理。
2. GET `http://ep-open.tangeopen.com/services`，读取并校验服务地址。
3. 无本地身份：POST `/v1/device/report`，其中 `mac` 是 IMEI 派生标识。显示返回的六位码，按上游协议使用本地 190 秒窗口。
4. 临时 MQTT 用 `temp_client_id` / `temp_token`，订阅 `device/{temp_client_id}/cmd`，等待 `auth_grant`。
5. 校验云设备 ID、设备密钥，写入 `tirtc_device_auth.nvm`，读回并检查结构、CRC 和内容，然后发布授权 ACK。写入或验证失败时不确认授权。
6. 用 UTC 秒数、随机 nonce 和设备密钥签名 POST `/v1/device/token`。签名使用真实 HMAC-SHA256；收到有效 `mqtt_token` 才认定平台 API 已鉴权。
7. 正式 MQTT 使用 `sn_{device_id}`，订阅命令和通知两个主题。连接与两项订阅完成后才报告平台在线。
8. 开机已有身份时直接重新鉴权。410 / 6006 或远程解绑进入有签名的重新绑定流程，保留持久化身份；临时掉线不会当成解绑。

当前媒体运行时由 `runtime` 独立启动，平台提供已绑定身份、业务请求与消息分发；AI、calls、LIVE、Room 各自处理自己的会话。IMEI 不作为云端下发的设备 ID。身份读取仅限后台生命周期所有者，可能查询 IMEI/ICCID，不能从 UI 调用或打印凭据。

Room 使用正式 CALL 服务上下文访问 `/v1/call/group/device/*`。正式命令主题中的 `type=room_assignment_changed` 将顶层版本和有效期字段交给 Room，由 Room 重新查询关系。鉴权、入房及媒体租约见 [代码与流程](../docs/代码与流程详解.md)。

## 设备信息与媒体能力

[device_metadata.h](device_metadata.h) 集中维护设备信息，由现有 `/v1/device/profile` 请求上报：

| 平台字段 | HM Pro 来源 |
|---|---|
| 芯片型号 `hardware.chip_model` | `TIRTC_DEVICE_CHIP_MODEL`，NT26F9D0 |
| 开发板型号 `hardware.board_model` | `TIRTC_DEVICE_BOARD_MODEL` |
| 固件版本 `firmware_version` | `TIRTC_DEVICE_FIRMWARE_VERSION`，`F9D_D_pro_0.1.0` |

已绑定设备在开机联网鉴权成功、媒体运行时就绪后更新 profile。应用包使用构建版本 `pro_f9d_d_wxav605_sdk170`。设备状态页的模组版本来自模组接口。[构建与固件](../docs/构建与固件.md)

`profiles.call` 声明设备互呼上行 Opus、下行 Opus 或 A-law，视频使用 MJPEG。`profiles.voip` 声明微信视频的 MJPEG 格式、A-law 下行音频及 240×240 显示配置。`profiles.stream` 使用 A-law 双向音频和 MJPEG 上行视频，对应的流 ID 为上行音频 10、上行视频 11、下行音频 14。各类业务按各自的媒体声明配置。[设备 profile 契约](https://github.com/tangeai/tirtc-server-example/blob/main/thing-connect/api-reference.md#post-v1deviceprofile)

## 接入条件

平台使用活动 SIM0，设备需取得 IP 和有效 UTC 时间。当前体验服务发现使用 HTTP，信令使用 MQTT，两条链路均为明文。正式连接和必需订阅成功后，设备页显示在线。设备身份、密钥和令牌由后台模块持有。

[设备绑定](../docs/当前设备操作.md#binding) · [模块来源与许可](SOURCE_PROVENANCE.md)
