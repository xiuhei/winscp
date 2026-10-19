# WinSCP 工作区

当前桌面程序使用 `qt/` 中的 C++20 / Qt 6 实现。连接 SSH 服务器后，双栏文件管理与底部交互终端自动打开，并复用同一个 SSH 连接。程序不包含官方网站、更新检查、广告、捐赠、遥测或作者介绍入口。

- [功能、构建与使用](qt/README.md)
- [功能精简清单与已知未生效配置](qt/FEATURE_REVIEW.md)

```powershell
.\qt\scripts\build.ps1 -Configuration Debug
.\qt\tests\run_integration_tests.ps1 -Configuration Debug
.\qt\scripts\deploy.ps1
```

发布包生成在 `qt/dist/`。原版 WinSCP 的 `source/`、`libs/`、`deployment/`、`dotnet/`、`translations/` 和根目录 `build.bat` 独立保留，不参与 Qt 构建。原版许可见 `license.txt`，Qt 实现许可见 `qt/LICENSE`。
