# 板级兼容代码

`appram.c` 配合应用链接布局初始化 RAM，`liot_lcdDev_ST7789.c` 提供本触屏板的显示驱动。`board_compat.mk` 使用外部 SDK 中与所选底包匹配的驱动，并选取本产品需要的替换目标。NT26F9D0 的引脚、链接和音频配置集中在 [boards/f9d_d](../boards/f9d_d/README.md)。

本目录保留原厂派生文件的版权声明及 [第三方许可](licenses)。 [产品许可](../THIRD_PARTY_NOTICES.md)
