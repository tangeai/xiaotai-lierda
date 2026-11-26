# 存储模块

HM Pro 的字体、图标、背景与表情随应用编译，构建选择内置资源后端。应用启动内部文件系统状态采集，平台身份和音频设置使用内部 NVM。[内置资源](../resource_store/README.md) · [音频偏好](../preferences/README.md)

本目录保留原 Pro 的 SPI Flash / LittleFS 实现及上游许可证。HM Pro 的内置资源构建不会编译这个外置存储服务，旧版外置资源安装和打包步骤适用于 NT26F6D0 的 PM Pro。 [PM Pro 外置存储说明](../../TiRTC-EC718PM-Pro/storage/README.md) · [LittleFS 许可证](littlefs/LICENSE.md)

HM Pro 的内部文件系统布局由固定的 F9D_D 底包提供。更新应用或迁移底包时，按本产品的下载范围和备份要求操作。[板级说明](../boards/f9d_d/README.md) · [构建与固件](../docs/构建与固件.md)
