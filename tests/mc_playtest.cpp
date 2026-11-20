#include "companion_host.hpp"
#include "application.hpp"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>

namespace {
// 体验服用已授权的单一查看参数调用正式工具，游戏事实只取自插件回执。
dfy::model::Response sample_turn(const dfy::model::Request& request) {
    using namespace dfy::model;
    const auto& last = request.messages.back();
    if (last.role == Role::tool || request.tools.size() != 1) return {{Role::assistant, "本轮查看已结束。"}};
    const auto newline = last.content.rfind('\n');
    if (newline == std::string::npos) return {{Role::assistant, "我在这里。"}};
    const auto args = nlohmann::json::parse(last.content.substr(newline + 1), nullptr, false);
    if (!args.is_object() || !args.contains("order") || !args.contains("place")) return {{Role::assistant, "我在这里。"}};
    return {{Role::assistant, "", {{"visit", request.tools.front().name, args.dump()}}}, FinishReason::tool_calls};
}
// 仅向离线探针记下真实进入后端的事实和替身选中的结果，供外部玩家对照。
void record_input(const std::filesystem::path& root, const nlohmann::json& record) {
    static std::mutex lock;
    std::lock_guard guard(lock);
    std::ofstream(root / "model-events.jsonl", std::ios::app | std::ios::binary) << record.dump() << '\n';
}

// 在固定外部输入下提供不同可控选择；输出文字不参与动作分支判定。
// 步骤：记录原始事件 → 选择继续或停止在途工作 → 按实际工具回执决定行动及是否发言。
dfy::model::Response partner_sample(const dfy::model::Request& request, const std::filesystem::path& root) {
    using namespace dfy::model;
    using Json = nlohmann::json;
    const auto& last = request.messages.back();
    const auto speak = [&](std::string message) {
        return Response{{Role::assistant, std::move(message)}, FinishReason::stop};
    };
    const auto call = [&](const std::string& skill, const Json& arguments) -> Response {
        const auto tool = std::ranges::find_if(request.tools, [&](const auto& def) { return def.name.ends_with("_" + skill); });
        if (tool == request.tools.end()) throw std::runtime_error("missing registered skill: " + skill);
        record_input(root, {{"call",skill},{"arguments",arguments}});
        return {{Role::assistant, "", {{skill, tool->name, arguments.dump()}}}, FinishReason::tool_calls};
    };
    if (request.tools.empty()) {
        const auto input = Json::parse(last.content);
        const bool stop = input.at("currentEvents").back().at("sequence") == 6;
        record_input(root, {{"check", input.at("newEvents")}, {"work",input.at("work")}, {"stop",stop}});
        return speak(stop ? "stop" : "continue");
    }
    if (request.tools.size() != 4 || request.messages.front().role != Role::system) throw std::runtime_error("invalid partner scope");
    if (last.role == Role::user) {
        const auto input = Json::parse(last.content);
        record_input(root, input);
        const auto& event = input.at("events").back();
        if (event.at("type") != "player.message") return speak("内部结算：暂时安静");
        const auto seq = event.at("sequence").get<unsigned>();
        if (seq == 2 || seq == 7) return call("reply", {{"text",root.parent_path().filename().string() + "-" + std::to_string(seq)}});
        if (seq == 3) return call("observe", {{"block","dirt"}});
        if (seq == 4 || seq == 6) return call("move", {{"x",seq == 4 ? 32 : 1},{"y",-60},{"z",-4}});
        if (seq == 8) throw std::runtime_error("fixture provider unavailable");
        return speak("内部结算：等待");
    }
    const auto result = Json::parse(last.content.substr(last.content.find('{')));
    if (result.at("status") != "done") return speak("内部结算：已读受阻结果");
    if (last.tool_call_id == "observe" && !result.at("visible").empty()) return call("collect", {{"block","dirt"},{"count",2}});
    return speak("内部结算：已读工作回执");
}
}

// 保持真人体验的正式宿主运行，向本地日志发布进展和计划板并完整收尾。
// 步骤：绑定原生包和独立数据目录 → 推进玩家委托与回执 → 按退出请求回收插件。
int wmain(int argc, wchar_t** argv) {
    using namespace dfy;
    try {
        const bool partner = argc == 4 && std::wstring_view(argv[3]) == L"--partner-probe";
        if (argc != 3 && !partner) throw std::invalid_argument("package and playtest directory required");
        const std::filesystem::path root = argv[2];
        app::Application host({}, root / "packages", root / "user", model::Backend{
            [partner, root](const auto&, const auto& request) { return partner ? partner_sample(request, root) : sample_turn(request); }});
        app::run_companion_host(host, argv[1], root, partner ? app::CompanionMode::partner : app::CompanionMode::scenario);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
