# 功能精简清单

“无用”取决于使用场景。下列清单区分当前未生效的配置、可选精简项和应保留的基础能力。默认只使用 `qt/` 构建桌面程序，原版 `source/` 不参与此构建。

## 当前没有实际作用的配置

| 配置/接口 | 当前行为 | 相关文件 |
| --- | --- | --- |
| SSH 压缩选项 | 会话编辑器读写 `compression`，SSH 握手没有应用它 | `src/ui/SessionEditor.cpp`、`src/ssh/SshConnection.cpp` |
| FTP 被动模式复选框、URL 的 `passive` 参数 | 只保存到 `m_passive`，数据连接始终使用 PASV/EPSV；取消勾选不会启用主动模式 | `src/proto/FtpBackend.cpp` |
| `agentForwarding` | 只参与会话序列化，没有请求代理转发；使用本地 SSH Agent 认证仍然有效 | `src/core/SessionData.cpp` |
| `maxConnections` | 只参与会话序列化；实际并发由全局 `parallelTransfers` 决定 | `src/core/SessionData.cpp`、`src/transfer/TransferManager.cpp` |
| `supportsResume()`、FTP `m_useRest` | 没有续传调用方，SFTP 接口声明支持也没有对应执行逻辑 | `src/core/Backend.h`、`src/proto/SftpBackend.h`、`src/proto/FtpBackend.cpp` |
| `conflictPolicy=resume` | 没有续传功能，实际按覆盖处理；界面不提供这个选项 | `src/transfer/TransferManager.cpp` |
| `rawSettings` | 仅保留序列化接口，当前连接和终端不读取；站点保存还会清除此字段 | `src/core/SessionData.cpp`、`src/core/Settings.cpp` |

这些项可以删除，或在实现对应能力后再暴露。它们不应被视为已支持的功能。

## 可按用途精简

| 功能 | 可删除的前提 | 影响 |
| --- | --- | --- |
| FTP、FTPS、WebDAV | 所有目标服务器均使用 SSH/SFTP | 对应后端、站点协议选项和部分 Qt Network 使用可以缩减 |
| SCP 后端 | 所有服务器支持 SFTP | 可移除 POSIX 命令拼接与文本列表解析 |
| 双向同步、删除目标多余文件 | 只做手动上传下载 | 可缩减同步界面和比较引擎 |
| 外部编辑器和关联程序打开 | 只用内置文本编辑器 | 可减少系统外部程序入口 |
| 最近会话、保存密码、自动保存界面 | 只用临时连接，且不需要记忆配置 | 可减少本地配置与凭据存储 |
| 演示、截图、图标导出命令 | 正式版不需要自动预览和打包检查 | 可从发布程序剥离，仅保留开发工具 |

## 原版中对当前桌面用途可能多余的模块

原版的官方网站/在线帮助、自动更新和下载、新闻与技巧页、捐赠与商店推广、使用情况上报和安装完成后的官网跳转，不属于当前 Qt 程序的构建内容。当前界面没有这些入口，也没有关于/作者介绍。

原版 `.NET` 程序集、命令行自动化、资源管理器拖放扩展、浏览器 URL 注册、双界面模式、云对象存储和多语言资源，只在明确不需要对应场景时才值得从原版工程中删除；它们不能普遍认定为完全无用。

## 应保留

SSH 主机密钥检查、TLS 证书验证、认证、密码加密、权限与时间戳、错误处理、取消、传输队列和连接保活直接影响安全或文件正确性。主界面显示的作者宣传可以删除，但项目和第三方依赖要求的许可证与版权声明应保留。
