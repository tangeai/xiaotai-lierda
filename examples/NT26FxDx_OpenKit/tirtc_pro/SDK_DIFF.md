# 当前 SDK 与应用边界

先从[利尔达官方仓库](https://github.com/lierda-iot/CAT1.bis_OpenCPU/tree/59f1250c4b8caad4bbf4f74167b7b39c79097f8c/)检出 `Release_V0107` SDK（含底包和工具），再下载小钛应用并运行合并命令 `python xiaotai-lierda/tools/setup_sdk.py --sdk-dir CAT1.bis_OpenCPU-sdk`。应用仓库与 SDK 并列，准备脚本校验固定提交 `59f1250c4b8caad4bbf4f74167b7b39c79097f8c`。SDK、底包、原厂工具及示例不在本仓库重复保存；产品差异集中在 `examples/NT26FxDx_OpenKit/tirtc_lite` 和 `tirtc_pro`。

两处 Makefile 接入产品：准备命令将 `tools/lierda-sdk.patch` 应用到外部 SDK 根 `Makefile`，分离产品输出目录与名称，并在并行编译前创建 `$(BUILDDIR)/components/precfg/src`，确保配置头文件和引脚代码生成器的输出目录已存在；`examples/NT26FxDx_OpenKit/Makefile` 选择 `APP_VARIANT=lite/pro` 并要求 `MODEMPKG=F6D_D`。不指定产品时仍使用原厂 app/demo 分支。官方复制的 `NT26FxDx_OpenKit_TiRTC` 目录未纳入本工程。

Pro 的配置、板级兼容代码、TiRTC 库、资源与构建入口均在本产品目录。公共 SDK 更新时，应检查 [板级依赖与 ABI 校验](board_compat/README.md)，不能只替换单个底包文件绕过版本检查。

先完成[源码准备](../../../README.md#编译与下载)，再在外部 SDK 根目录执行：

```powershell
.\examples\NT26FxDx_OpenKit\tirtc_pro\build.bat
```

产物与维护入口见 [构建与固件](docs/构建与固件.md)。`gccout/` 和 `tools/toolchain/gcc/` 是构建时生成的目录，清理后可由构建流程重新生成。
