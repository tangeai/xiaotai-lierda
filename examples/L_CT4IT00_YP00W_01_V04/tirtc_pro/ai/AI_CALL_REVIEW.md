# AI 呼叫复核：47 版（2026-09-23）

## 参考基线

以同芯片利尔达工程的已验证流程为主，ESP32 用于核对消息兼容性，不移植其硬件、媒体或 UI 实现。

- [利尔达 ai_chat.c，f159bfd](https://github.com/tangeai/xiaotai-lierda/blob/f159bfd3e82cc514dfe6b5b2269b2c96fa8e8f93/examples/L_CT4IT00_YP00W_01_V04/tirtc/src/features/ai_chat.c)：新查询票据、唯一联系人、回包完整提交后转接。
- [ESP32 P4 starter_runtime.c，ed1bcf7](https://github.com/tangeai/xiaotai-esp32/blob/ed1bcf78ebe9ffac2c67e8a75d80c9cf31ead831/waveshare-esp32p4-xiaotai/components/starter_runtime/src/starter_runtime.c)：device_action/旧式呼叫意图和字段别名。
- 两个参考实现的 AI 呼叫路径均按语音设计；本设备已具备经过实测的视频通话，因此保留应用默认视频、显式 audio/voice 语音的规则。

## 46 版复现的问题及修正

| 问题 | 触发与影响 | 47 版处理 |
| --- | --- | --- |
| 旧式呼叫指令拒绝 | call_intent / ai_call_intent 回传仅支持 AI 语音对话，平台可能据此口头拒绝 | 去掉过时能力说明；有效旧式请求复用原安全拨号路径 |
| 字段未完整兼容 | target_name 被判为缺失目标；channel/source 等未参与类型筛选 | 补充字段和 start_voip_call 别名；未知类型仍拒绝 |
| type 与 media 混用 | type=wechat、media=audio 原先会变成默认视频 | 显式媒体字段优先，联系人路由单独解析 |
| 数字编号无回包 | ESP32 可处理数字 RPC id，本地原先直接忽略 | 支持精确整数，按数字原样回传，与同文本字符串编号区分 |

名称与 target_id 同时存在时再核对最终联系人 ID，避免字段不一致导致误拨。目标仍必须属于实时查询的授权联系人，不能直接拨打模型提供的任意 ID。
以上属于应用入口兼容问题，不等于已经证明用户那一轮口头拒绝由哪一个触发；仍需检查实机命令和云端角色。

## 保持并验证的流程

1. AI 回调只复制命令，由原 AI 线程解析；所有消息仍有大小及 JSON 深度边界。
2. 当前有效 AI 会话才处理指令。每次呼叫发起新联系人查询票据，HTTP 留在原联系人工作线程，AI 采播继续运行。
3. 完整名称或 ID 必须唯一命中；同名、离线、请求过期、列表不完整、名称/ID 不一致均不拨号。
4. accepted 回包须完整交给 SDK（返回长度包含 4 字节命令字），之后才排队转入原 calls 模块。
5. 回包期间若取消、换绑定、来电抢占，排队前再次检查状态。calls 模块要求 AI 音频和 RTC 资源释放后才能接管。
6. 微信仍走原 wx_room_type，设备仍走原 call_type；视频/语音决定原媒体启动分支和通话页面。
7. 挂断、拒接、超时、来电和远程查看的处理代码保持原样；没有增加媒体线程、图像缓冲或 SDK 修改。

## 验证与边界

- 修改前已在真实解析代码复现：target_name 报 invalid_params；type=wechat + media=audio 被选成 video；数字 id 被忽略。
- 修改后：314 个应用回归用例（AI 217、通话 63、联系人 34），11 个联系人查询用例，56 条原协议断言和 76 条跨 Demo 断言通过。
- 测试覆盖取消、网络/绑定变化、查询失败、迟到响应、默认视频、显式音频、旧式指令、同名/离线、编号类型保留及 ID 不一致。
- ARM 严格语法检查、整包编译通过；代码改动限定在 tirtc 内的 AI 模块和版本配置，共用 SDK、calls、contacts、视频、音频和 UI 源码未改。
- 主机测试模拟网络与硬件边界，不能代替真实平台及设备接听测试；本次未烧录、未发起真实呼叫、未改云端角色。
- 联系人表仍最多 12 条，AI 查询仍有 12 秒期限；列表超限/不完整返回 contacts_unavailable，不用截断列表猜目标。
- 推荐平台继续使用利尔达标准 device_action/call_contact；见 [AI_CALL.md](AI_CALL.md)。角色和插件描述不能仍限制为仅语音。
