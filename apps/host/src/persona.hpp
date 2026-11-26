#pragma once
#include <dafeiyu/run_options.hpp>
#include <filesystem>

namespace dfy::app {
// 本体持有的身份和说话方式，与场景、工具和玩家上下文独立。
struct PersonaProfile {
    std::string version, instructions;
    // 为实际输出渠道固定规则模板，面向用户的入口单独绑定人格版本。
    void apply(agent::RunOptions& options, agent::OutputRoute route) const;
};

// 从本体数据目录读取可选人格档案，缺省使用内置版本。
PersonaProfile load_persona(const std::filesystem::path& user_data);
}
