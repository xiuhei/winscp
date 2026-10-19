# 第三方组件

Qt 采用动态链接，使用其 LGPLv3 许可选项。用户可以替换兼容的 Qt 动态库，也可以使用本目录的构建脚本重新链接应用。Qt 自身及内含第三方组件的许可文本保存在 `licenses/qt/`；发布包同时包含当前 Qt 安装提供的 SPDX 清单，以标识构建版本及源码位置。

- Qt 源码与构建说明：[qtbase](https://code.qt.io/cgit/qt/qtbase.git/)、[qtsvg](https://code.qt.io/cgit/qt/qtsvg.git/)。使用与发布包 SPDX 清单对应的提交。
- libssh2：`licenses/libssh2/LICENSE.txt`。
- OpenSSL：`licenses/openssl/LICENSE.txt`。
- zlib：`licenses/zlib/LICENSE.txt`。

这些文件是软件分发所需的法律声明，不作为程序中的作者介绍或推广入口。
