#include "support/replay_material.hpp"
#include <iostream>

namespace {
using namespace dfy::tests;
// 报告文件材料与受控重跑范围不符。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证坏材料在解释阶段拒绝，不执行降级路径。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid replay material accepted");
}
// 拒绝未知入口、重复字段、错误类型、版本和缺少预期。
void invalid(const material::Json& original) {
    auto value = original; value["schema_version"] = 2; rejects([&] { parse_replay_material(value.dump()); });
    value = original; value["endpoint"] = "https://example.invalid"; rejects([&] { parse_replay_material(value.dump()); });
    value = original; value["fixtures"][0]["plugin_path"] = "production.dll"; rejects([&] { parse_replay_material(value.dump()); });
    value = original; value["fixtures"][0]["id"] = "production/tool"; rejects([&] { parse_replay_material(value.dump()); });
    value = original; value["expected"].erase("model_requests"); rejects([&] { parse_replay_material(value.dump()); });
    value = original; value["policy"]["tool_limit"] = 1.5; rejects([&] { parse_replay_material(value.dump()); });
    value = original; value["timeout_ms"] = -1; rejects([&] { parse_replay_material(value.dump()); });
    value = original; value["decisions"] = {"automatic"}; rejects([&] { parse_replay_material(value.dump()); });
    const auto text = original.dump(); rejects([&] { parse_replay_material("{\"schema_version\":1,"+text.substr(1)); });
    rejects([&] { parse_replay_material(std::string(2*1024*1024+1,' ')); });
}
}
// 读取固定 JSON 夹具后重跑两次，文件无法指定真实模型或任意执行函数。
// 步骤：读取明确夹具 → 两次重跑核对行为 → 验证错误材料拒绝 → 报告结果。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2,"expected fixed material path"); const auto scenario = load_replay_material(argv[1]);
        const auto first = replay_scenario(scenario), second = replay_scenario(scenario);
        check(first.tool_calls == 1 && second.tool_calls == 1 && first.effects == second.effects && first.model.consumed == 2,
            "file replay changed deterministic facts");
        std::ifstream file(std::filesystem::path(argv[1]),std::ios::binary); material::Json original; file >> original; invalid(original);
        std::cout << "Replay material checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
