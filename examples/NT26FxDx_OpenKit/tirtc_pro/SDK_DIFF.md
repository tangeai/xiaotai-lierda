# 当前 SDK 与应用边界

公共 SDK 对照维护者提供的 `org_1009/CAT1.bis_OpenCPU-main`。`components/`、`config/`、`rules/`、`docs/`、原厂工具及示例内容保持官方版本；产品差异集中在 `examples/NT26FxDx_OpenKit/tirtc_lite` 和 `tirtc_pro`。

两处公共 Makefile 接入产品：根 `Makefile` 分离产品输出目录与名称；`examples/NT26FxDx_OpenKit/Makefile` 选择 `APP_VARIANT=lite/pro` 并要求 `MODEMPKG=F6D_D`。不指定产品时仍使用原厂 app/demo 分支。官方复制的 `NT26FxDx_OpenKit_TiRTC` 目录未纳入本工程。

Pro 的配置、板级兼容代码、TiRTC 库、资源与构建入口均在本产品目录。公共 SDK 更新时，应检查 [板级依赖与 ABI 校验](board_compat/README.md)，不能只替换单个底包文件绕过版本检查。

在工程根目录执行：

```powershell
.\examples\NT26FxDx_OpenKit\tirtc_pro\build.bat
```

产物与维护入口见 [构建与固件](docs/构建与固件.md)。`gccout/` 和 `tools/toolchain/gcc/` 是构建时生成的目录，清理后可由构建流程重新生成。
