#pragma once
#include <dafeiyu/plugin_instance.hpp>
#include <dafeiyu/tool_work.hpp>

namespace dfy::plugins {
// 创建不持有 DLL 的宿主工厂，实际调用时绑定原实例并持有能力直到工作释放。
agent::ToolFactory native_tool_factory(std::weak_ptr<PluginInstance> instance, std::string local_id, std::uint32_t version);
}
