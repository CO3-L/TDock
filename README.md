# TDock

TDock 为 Windows 10 原生任务栏添加图标悬停放大、点击弹跳和居中。支持 Windows 10 22H2 x64 的经典主屏底部任务栏。动画需要已适配的资源管理器构建。

[下载最新免安装版](https://github.com/CO3-L/TDock/releases/latest)

## 使用

下载 ZIP 后完整解压，双击 `TDock.exe`。点击系统托盘中的 TDock 图标可调整设置。首次运行默认开启「动画」和「居中」，「开机自启」默认关闭。

## 卸载

运行解压目录里的 `TDock-Uninstall.exe`。如果缓存 DLL 被占用，可在确认后重启资源管理器，或选择下次登录 Windows 时自动清理。详细步骤见 [免安装版说明](README-portable.md)。

## 从源码构建

在 Windows 上准备 Visual Studio 2022 x64 C++ 工具、Windows SDK、CMake 3.20 或更新版本，以及 .NET Framework 4.8。然后在项目目录运行以下命令。

```powershell
.\Build-TDock.ps1
.\Build-TDockPortable.ps1
```

输出位于 `dist/`，包括免安装 ZIP 和对应的 SHA-256 校验文件。

## 许可

TDock 采用 [MIT 许可证](LICENSE)。MinHook 1.3.4 的原许可和作者说明位于 [third_party/minhook-1.3.4](third_party/minhook-1.3.4)。
