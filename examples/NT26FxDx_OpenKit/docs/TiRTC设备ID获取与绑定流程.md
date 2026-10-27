# TiRTC 设备端：ID 获取与两种接入方式

本文适用于本仓库的 **Lite 和 Pro**。两版的设备身份与平台绑定主流程一致，源码位置见文末。

## 方式一：申请 ID 和密钥，写入设备后接入

**获取方式：**联系探鸽商务，发邮件至 `business@tange.ai` 申请开通。官方申请说明列出的设备凭据是测试设备的 `device_id`（设备 ID）和 `device_secret_key`（配套密钥）；量产设备的分配方式需与平台确认。[申请说明](https://docs.tange.ai/products/tirtc/get-started/apply-for-access.html)

**设备流程：**将申请到的 ID/密钥写入设备 → 开机读取 → 联网 → 按顺序调用以下 SDK 接口。

以下片段仅展示核心身份参数的调用顺序，实际实现须检查各接口返回值并处理启动事件。

```c
// device_id：申请得到的设备 ID。
// device_secret_key：申请得到的配套设备密钥。
// client_id：设备自身的稳定硬件标识，例如 IMEI、MAC 或生产序列号。
// callbacks：设备程序定义的 TIRTCCALLBACKS 回调结构体。

TiRtcInit();  // 初始化 SDK
TiRtcSetOption(TIRTC_OPT_DEVICE_SECRET_KEY,
               device_secret_key, (uint32_t)strlen(device_secret_key));
TiRtcSetOption(TIRTC_OPT_CLIENT_ID,
               client_id, (uint32_t)strlen(client_id));
TiRtcStart(device_id, &callbacks);  // 使用申请到的 ID 启动 SDK
```

`strlen()` 传入前一个字符串的字节长度，不含末尾的 `\0`。`client_id` 由设备提供并保持稳定，同一设备 ID 后续启动应沿用首次成功启动时的 `client_id`；`&callbacks` 是程序中回调结构体的地址，回调结构体须在 SDK 运行期间保持有效。`TiRtcStart()` 返回 0 不代表已启动成功，收到 `TIRTC_EVENT_SYS_STARTED` 才表示 SDK 启动完成。[SDK 参数说明](https://docs.tange.ai/products/tirtc/guides/connection.html)

当前 Lite 和 Pro 默认实现方式二；采用方式一需要适配凭据写入和启动流程，并另行对接所需的平台业务。

## 方式二：领取六位验证码，等待平台下发 ID 和密钥

**适用范围：**当前 Lite 和 Pro 新设备首次接入其配套设备管理平台。HTTP/MQTT 服务由平台提供，嵌入式端负责发请求、处理消息和保存凭据；这套绑定流程不是所有 TiRTC 项目的必需步骤。[接入职责说明](https://docs.tange.ai/products/tirtc/guides/development-overview.html)

**设备流程：**联网 → 获取并显示六位码 → 等待用户完成绑定、平台下发授权 → 收到并保存 ID/密钥 → 建立正式 MQTT 连接 → 启动 TiRTC SDK。

**设备端按顺序调用的接口和代码：**

| 步骤 | 接口或消息 | 用途与对应代码 |
| --- | --- | --- |
| 1. 发现服务 | `GET http://ep-open.tangeopen.com/services` | 获取设备管理、MQTT 和 TiRTC 服务地址。代码：`binding_discover_services()` |
| 2. 领取六位码 | `POST /v1/device/report` | 上报设备伪 MAC，取得六位码 `data.code`、临时 MQTT 标识 `temp_client_id` 和凭据 `temp_token`。代码：`binding_report_device(false)` |
| 3. 等待授权 | 连接临时 MQTT，订阅 `device/{temp_client_id}/cmd` | 订阅成功后显示六位码，等待平台授权。代码：`binding_wait_for_grant()` |
| 4. 接收 ID/密钥 | MQTT `auth_grant` 消息 | 读取 `payload.device_id` 和 `payload.device_key`，写入设备并读回校验，通过临时 MQTT 发送授权确认，随后清理临时 MQTT。代码：`binding_mqtt_incoming_cb()` 及其解析函数、`binding_wait_for_grant()` |
| 5. 获取正式 MQTT 凭据 | `POST /v1/device/token` | 使用收到的 ID/密钥进行签名认证，取得 `mqtt_token`。代码：`binding_verify_server_binding()` |
| 6. 建立正式 MQTT 连接 | MQTT 连接及主题订阅 | 使用设备 ID 和 `mqtt_token` 连接，完成订阅后设置绑定就绪状态。代码：`binding_start_formal_mqtt()` |

表中 `binding_xxx()` 都是设备端示例内部函数，主流程在 `device_binding.c` 的 `demo_binding_task()` 中；移植时不要求使用相同函数名。两个 POST 接口使用步骤 1 返回的设备管理服务地址。六位码由服务器返回；正式 ID/密钥从步骤 4 的授权消息取得。

**已绑定设备重启：**读取已保存的 ID/密钥、发现服务，再调用 `/v1/device/token` 校验平台绑定状态；校验成功后直接建立正式 MQTT 连接，无需重新领取六位码。只有平台明确返回已解绑（HTTP 410 且业务码为 6006），才使用已有凭据签名调用 `/v1/device/report` 重新绑定；其他错误进入重试。

Lite 和 Pro 已有这些逻辑，可按目标硬件和后台环境复用、适配。客户接入前需确认平台服务地址、接入权限及签名规范；换硬件时需适配 HTTP、MQTT 和凭据存储。

绑定就绪后，TiRTC 任务通过 `demo_binding_get_tirtc_identity()` 读取身份，再进入 `runtime_start()`。核心 SDK 调用如下：

```c
// identity->device_id：auth_grant 下发并保存的设备 ID。
// identity->device_key：auth_grant 下发并保存的设备密钥。
// identity->client_id：设备根据 IMEI 生成的稳定伪 MAC。
// s_callbacks：tirtc_runtime.c 中定义的 TIRTCCALLBACKS 回调结构体。

TiRtcInit();
TiRtcSetOption(TIRTC_OPT_DEVICE_SECRET_KEY,
               identity->device_key, (uint32_t)strlen(identity->device_key));
TiRtcSetOption(TIRTC_OPT_CLIENT_ID,
               identity->client_id, (uint32_t)strlen(identity->client_id));
TiRtcStart(identity->device_id, &s_callbacks);
```

收到 `TIRTC_EVENT_SYS_STARTED` 表示 SDK 启动完成。`mqtt_token` 用于正式 MQTT 认证，也用于配套平台业务 HTTP 请求的 `Authorization: Bearer` 鉴权，不能替代这里的设备密钥；`client_id` 也不是临时 MQTT 的 `temp_client_id`。

代码仅展示核心身份参数调用。Lite 和 Pro 的完整流程还检查绑定就绪、服务地址及 ICCID，在 `TiRtcInit()` 前配置发送缓冲区；其他网络选项、返回值处理和启动事件见各自的 `runtime_start()` 及运行时回调。

## 源码入口

| 产品 | 设备绑定与凭据 | SDK 配置与启动 |
| --- | --- | --- |
| Lite | [src/services/device_binding.c](../tirtc_lite/src/services/device_binding.c) | [src/core/tirtc_runtime.c](../tirtc_lite/src/core/tirtc_runtime.c) |
| Pro | [platform/device_binding.c](../tirtc_pro/platform/device_binding.c) | [runtime/tirtc_runtime.c](../tirtc_pro/runtime/tirtc_runtime.c) |
