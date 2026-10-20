# WinSCP Qt

Windows 文件管理器，使用 C++20、Qt 6 Widgets、libssh2 和 Qt Network。应用没有官方网站入口、更新检查、广告、捐赠、遥测或作者介绍。网络访问仅用于用户配置的服务器连接及其协议操作；帮助菜单提供本地快捷键和日志。

默认使用 Windows 风格的浅色紧凑界面：16px 图标、20px 文件行高、方角面板和小型工具栏。深色主题也使用相同的紧凑布局，终端采用黑色背景。

## 功能

- SFTP、SCP、FTP、显式/隐式 FTPS、HTTP/HTTPS WebDAV，以及本地文件浏览。
- 双栏文件管理、`..` 上一级目录、可点击的各级路径、路径历史、筛选、隐藏文件、拖放上传下载、新建、重命名、递归删除、属性、Unix 权限和符号链接。
- 站点和最近会话、按站点恢复上次使用的本地和远程目录、Windows DPAPI 密码保存、SSH 主机密钥验证。
- 后台传输队列、并发任务、进度、取消、冲突确认、修改时间和权限保留。
- 本地到远程、远程到本地及双向目录比较，预览后同步。
- 内置文本编辑器，保存远程文件后上传；深色和浅色主题。

## 导航、刷新与打开文件

路径栏的各级目录可以直接点击；铅笔按钮或 Ctrl+L 可输入完整路径。文件列表中的 `..` 返回上一级，不参与上传、下载或删除。本地与远程面板之间拖放文件或目录会执行真实传输，放到文件夹行时以该文件夹为目标，放到空白处时以当前目录为目标。

远程面板右上角的“自动刷新”开启后每 2 秒刷新当前目录，保留选择与滚动位置；加载或文件操作期间暂缓刷新。站点编辑不提供初始目录选项，成功浏览的双端目录自动记忆，重新打开站点时恢复；记忆的远程目录不存在时回到服务器主目录。

双击远程文件会使用独立的进度窗口下载到程序旁的 `downloads/<缓存标识>/content/`，完成后调用系统关联程序打开。右键“编辑”使用内置编辑器并支持保存上传。缓存通过协议、服务器、端口、用户、完整路径、大小和修改时间区分，每次打开都重新检查远程属性，并校验本地大小和修改时间；远程缺少修改时间时不复用缓存。下载后再次检查远程属性，避免在源文件变化时打开不完整版本。服务器内容变化但大小和修改时间均未变化时，无法仅靠这些属性识别。

“首选项 → 传输 → 远程打开缓存保留”设置缓存保留天数，默认 7 天，从最后打开时间计算。在启动、打开文件、修改设置以及每小时检查时清理过期缓存，只处理程序管理的缓存子目录。

## 终端

连接 SFTP/SCP 服务器后，文件管理和底部终端自动打开。底部面板不能折叠，可拖动分隔条调整高度；Ctrl+T 或面板的新建按钮增加终端标签。

文件浏览与所有终端标签复用同一个 SSH 会话及 TCP 连接，仅打开独立的 PTY Shell 通道，不重新登录。libssh2 的操作统一在文件会话工作线程执行，终端通过定时器读取；文件操作期间暂缓终端轮询。后台传输任务使用独立连接，避免大文件传输阻塞交互。

终端工具栏的“自动同步目录”默认关闭，开启后文件面板与当前终端双向同步目录。Bash 和 Zsh 使用当前 Shell 中的提示符钩子报告目录，不修改服务器配置文件；也接收 OSC 7 目录报告。输入或运行命令期间，面板发起的切换会等待下一个提示符。其他 Shell 需要自行提供 OSC 7 报告。

支持 UTF-8、中文输入法、Tab 补全、方向键、Ctrl+C 等控制键、复制粘贴、括号粘贴、滚动缓冲区、字体缩放、ANSI 16/256/真彩色、光标、滚动区域、备用屏幕和 PTY 尺寸同步。终端获得焦点时，控制键发送给远端。关闭一个终端不关闭文件连接；断开或切换文件会话会关闭所属终端。FTP/FTPS/WebDAV 不支持 SSH Shell。

## 构建与发布

依赖 Windows 10/11 x64、Visual Studio 2022、Qt 6.5+、CMake 3.21+ 和 vcpkg 的 libssh2/OpenSSL。默认路径为 `C:\environment\Qt\6.10.3\msvc2022_64` 和 `C:\environment\vcpkg`，可通过 `DBGATE_QT_ROOT`、`VCPKG_ROOT` 或脚本参数覆盖。

```powershell
.\qt\scripts\build.ps1 -Configuration Debug
.\qt\scripts\deploy.ps1 -PackageName WinSCPQt-enhanced-win64
```

开发程序在 `qt/build/Debug/WinSCPQt.exe`。便携发布包在 `qt/dist/WinSCPQt-enhanced-win64.zip`，解压后运行 `WinSCPQt.exe`；程序目录下的 `portable.ini` 使配置保存在 `data/`。不要覆盖已有便携目录中的 `data/` 或 `downloads/`。

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

测试覆盖五种协议的连接、列表、上传、下载和删除；递归传输队列；同步；终端屏幕和按键；SFTP/SCP 与两个终端的连接身份一致性；同时读写文件；关闭 Shell 后继续浏览；断线与重连；真实 OpenSSH 的 PTY 尺寸、Ctrl+C 和 vi 备用屏幕；主窗口自动打开终端及切换到 FTP；路径导航和上一级目录；终端与文件面板双向目录同步；2 秒自动刷新；拖放事件触发真实上传下载；远程打开缓存复用、失效和过期清理；按站点恢复目录。

## 范围与限制

- 不是原版 WinSCP 的全部功能移植：不含 S3、.NET 程序集、原版脚本语言、会话导入和主密码。
- SCP 使用 SSH exec 的 POSIX 命令及文件流，需要服务器提供相应命令，不适用于受限的纯 SCP 协议端点。
- 不支持断点续传、FTP 主动模式、SSH 压缩、代理转发、跳板和端口转发；相关未生效配置见 [功能精简清单](FEATURE_REVIEW.md)。
- 终端实现 VT/xterm 常用子集，不具备 Xshell 的全部协议、会话和鼠标报告功能。

## 结构与许可

`src/core` 为协议接口、会话控制与配置；`src/proto` 为协议实现；`src/ssh` 为 SSH 连接；`src/terminal` 为 PTY、屏幕解析和渲染；`src/ui` 为界面；`src/transfer` 和 `src/sync` 为传输与同步。

本目录实现使用 `LICENSE`。发布时保留依赖所要求的许可文件。仓库根目录中的原版 WinSCP 源码和许可证独立保留。
