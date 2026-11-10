# 第三方依赖

原生依赖在 CMake 配置阶段获取，固定版本和 SHA-256 见 `cmake/Dependencies.cmake`。

| 依赖 | 版本 | 用途 | 许可/来源 |
|---|---|---|---|
| nlohmann/json | 3.12.0 | 配置与插件 JSON 协议 | MIT，https://github.com/nlohmann/json |
| SQLite | 3.54.1 | 核心和插件持久状态 | Public domain，https://www.sqlite.org/copyright.html |

nlohmann/json 许可随安装包发布。SQLite 为原生 C 实现，直接编译官方 amalgamation；没有 Python、Node.js 或 DSH 运行时依赖。Windows 使用所选 MSVC 工具链的 C/C++ 运行库，部署机器需具备匹配运行库；Debug 构建仅用于开发验证，部署应使用 Release 构建。
