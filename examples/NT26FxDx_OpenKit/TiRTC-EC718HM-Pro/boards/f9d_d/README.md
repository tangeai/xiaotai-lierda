# NT26F9D0 板级配置

[返回 TiRTC-EC718HM-Pro](../../README.md) · [ABI 校验](ABI.md)

| 项目 | 配置 |
| --- | --- |
| 模组 | NT26F9D0（EC718HM） |
| 板级配置 | `nt26f9d0_prodisplay` |
| 底包 | `Release_V0107` 自带的 `F9D_D` 1.7.0 |
| 显示 | 320×240 横屏 RGB565，ST7789，FT6336 触摸 |
| 摄像头 | GC032A，通过 CSPI 采集 |
| 音频 | 底包 3A 连续采播，应用通过 `media` 管理录放音和音量 |
| 界面资源 | 随应用编译，使用底包原生分区 |

引脚分配由 [iodriver.ini](iodriver.ini) 定义，链接布局由 [app.ld](app.ld) 定义。使用不同屏幕、触摸或摄像头的板卡时，依据实际连线和原厂接口复核这些文件再编译。 [构建与固件](../../docs/构建与固件.md)
