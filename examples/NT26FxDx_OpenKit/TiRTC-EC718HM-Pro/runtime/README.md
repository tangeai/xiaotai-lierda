# TiRTC 运行时适配

本目录连接 TiRTC SDK 与 OpenKit 的网络、时间、线程、内存和协议接口。各业务通过所有权申请和会话代次使用共享连接与媒体资源。退出时先停止对应媒体及 SDK 回调，再释放本次会话。

HM 构建使用共用 OpenKit SDK 1.7.0 自带的 `F9D_D`。准备命令提供 TiRTC 2.5.0 mini。[runtime.mk](runtime.mk) 验证 SDK 接口和依赖。 [F9D_D ABI](../boards/f9d_d/ABI.md) · [来源和许可](SOURCE_PROVENANCE.md)
