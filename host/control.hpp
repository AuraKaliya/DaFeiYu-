#pragma once
#include <dafeiyu/core.hpp>
#include <atomic>
namespace dfy::host {
void serve(InstanceRuntime& runtime, const Path& directory, const std::atomic<bool>& interrupted);
Json send(const Path& directory, const std::string& command, const Json& arguments, std::string command_id = {}, int timeout_ms = 10000);
}
