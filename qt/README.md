# WinSCP Qt

Windows 文件管理器，使用 C++20、Qt 6 Widgets、libssh2 和 Qt Network。应用没有官方网站入口、更新检查、广告、捐赠、遥测或作者介绍。网络访问仅用于用户配置的服务器连接及其协议操作；帮助菜单提供本地快捷键和日志。

## 功能

- SFTP、SCP、FTP、显式/隐式 FTPS、HTTP/HTTPS WebDAV，以及本地文件浏览。
- 双栏文件管理、路径历史、筛选、隐藏文件、拖放上传下载、新建、重命名、递归删除、属性、Unix 权限和符号链接。
- 站点和最近会话、Windows DPAPI 密码保存、SSH 主机密钥验证。
- 后台传输队列、并发任务、进度、取消、冲突确认、修改时间和权限保留。
- 本地到远程、远程到本地及双向目录比较，预览后同步。
- 内置文本编辑器，保存远程文件后上传；深色和浅色主题。

## 终端

连接 SFTP/SCP 服务器后，文件管理和底部终端自动打开。底部面板不能折叠，可拖动分隔条调整高度；Ctrl+T 或面板的新建按钮增加终端标签。

文件浏览与所有终端标签复用同一个 SSH 会话及 TCP 连接，仅打开独立的 PTY Shell 通道，不重新登录。libssh2 的操作统一在文件会话工作线程执行，终端通过定时器读取；文件操作期间暂缓终端轮询。后台传输任务使用独立连接，避免大文件传输阻塞交互。

支持 UTF-8、中文输入法、Tab 补全、方向键、Ctrl+C 等控制键、复制粘贴、括号粘贴、滚动缓冲区、字体缩放、ANSI 16/256/真彩色、光标、滚动区域、备用屏幕和 PTY 尺寸同步。终端获得焦点时，控制键发送给远端。关闭一个终端不关闭文件连接；断开或切换文件会话会关闭所属终端。FTP/FTPS/WebDAV 不支持 SSH Shell。

## 构建与发布

依赖 Windows 10/11 x64、Visual Studio 2022、Qt 6.5+、CMake 3.21+ 和 vcpkg 的 libssh2/OpenSSL。默认路径为 `C:\environment\Qt\6.10.3\msvc2022_64` 和 `C:\environment\vcpkg`，可通过 `DBGATE_QT_ROOT`、`VCPKG_ROOT` 或脚本参数覆盖。

```powershell
.\qt\scripts\build.ps1 -Configuration Debug
.\qt\scripts\deploy.ps1
```

开发程序在 `qt/build/Debug/WinSCPQt.exe`。便携发布包在 `qt/dist/WinSCPQt-1.0.0-win64.zip`，解压后运行 `WinSCPQt.exe`；程序目录下的 `portable.ini` 使配置保存在 `data/`。不要覆盖已有便携目录中的 `data/`。

```powershell
WinSCPQt.exe
WinSCPQt.exe --url "sftp://user@server:22/home/user"
WinSCPQt.exe --demo
WinSCPQt.exe --demo --screenshot preview.png
```

支持 `sftp://`、`scp://`、`ftp://`、`ftps://`、`webdav://`、`https://`。`insecure=1` 关闭 TLS 证书验证，`implicit=1` 使用隐式 FTPS。默认配置使用 Windows 应用配置目录；`WINSCPQT_HOME` 可覆盖配置和数据目录。

## 验证

```powershell
.\qt\tests\run_integration_tests.ps1 -Configuration Debug
```

需要 Docker Desktop 的 Linux 容器。脚本构建 `winscpqt-tests:local`，创建仅向本机暴露端口的 `winscpqt-tests-<PID>` 容器，运行协议测试服务和真实 OpenSSH。结束时自动删除测试容器，不创建持久 Docker volume。测试输出和服务日志保存在 `qt/build/integration-*`。

测试覆盖五种协议的连接、列表、上传、下载和删除；递归传输队列；同步；终端屏幕和按键；SFTP/SCP 与两个终端的连接身份一致性；同时读写文件；关闭 Shell 后继续浏览；断线与重连；真实 OpenSSH 的 PTY 尺寸、Ctrl+C 和 vi 备用屏幕；主窗口自动打开终端及切换到 FTP。

## 范围与限制

- 不是原版 WinSCP 的全部功能移植：不含 S3、.NET 程序集、原版脚本语言、会话导入和主密码。
- SCP 使用 SSH exec 的 POSIX 命令及文件流，需要服务器提供相应命令，不适用于受限的纯 SCP 协议端点。
- 不支持断点续传、FTP 主动模式、SSH 压缩、代理转发、跳板和端口转发；相关未生效配置见 [功能精简清单](FEATURE_REVIEW.md)。
- 终端实现 VT/xterm 常用子集，不具备 Xshell 的全部协议、会话和鼠标报告功能。

## 结构与许可

`src/core` 为协议接口、会话控制与配置；`src/proto` 为协议实现；`src/ssh` 为 SSH 连接；`src/terminal` 为 PTY、屏幕解析和渲染；`src/ui` 为界面；`src/transfer` 和 `src/sync` 为传输与同步。

本目录实现使用 `LICENSE`。发布时保留依赖所要求的许可文件。仓库根目录中的原版 WinSCP 源码和许可证独立保留。
