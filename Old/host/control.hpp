#pragma once
#include <dafeiyu/core.hpp>
#include <atomic>

namespace dfy::host {
// 运行本地文件控制通道，并在停止时关闭实例。
void serve(InstanceRuntime& runtime, const Path& directory, const std::atomic<bool>& interrupted);
// 通过当前实例的文件端点提交命令并等待响应。
Json send(const Path& directory, const std::string& command, const Json& arguments, std::string command_id = {},
    int timeout_ms = 10000);
}
