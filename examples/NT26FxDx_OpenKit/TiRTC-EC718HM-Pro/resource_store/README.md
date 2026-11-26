# HM Pro 的内置界面资源

NT26F9D0 使用 `TIRTC_UI_ASSET_MODE=embedded`。中文字库、数字字形、背景、电话与微信图标、表情姿态和动画时序都由应用中的源码提供。编译器将资源链接到正式应用包。使用利尔达原厂工具烧录。已安装匹配 `F9D_D` 1.7.0 底包和分区的设备，可按原厂工具流程仅更新 APP。 [构建与固件](../docs/构建与固件.md)

`embedded_assets.c` 检查链接后的字库、图片长度与表情描述，并将状态交给 UI。字形和背景指向应用中只读的资源数据，界面可以同步读取。字库的上游来源和更新方式见字体目录。[字体来源](../ui/tools/font-sources/SOURCE.md)

此目录也保留原 Pro 外置资源实现和其工具。外置资源安装器及合并包由 PM Pro 的构建入口提供。 [PM Pro 外置资源说明](../../TiRTC-EC718PM-Pro/resource_store/README.md)
