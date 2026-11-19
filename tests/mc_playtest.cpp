#include "companion_host.hpp"
#include "application.hpp"
#include <algorithm>
#include <iostream>
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
// 用固定选择检验原话、工具发现、真实结果反馈和跨回合上下文，隔离外部模型服务。
// 步骤：核对当前消息与工具 → 根据上一步实际结果选择下一技能 → 返回聊天或交付结论。
dfy::model::Response partner_sample(const dfy::model::Request& request) {
    using namespace dfy::model;
    using Json = nlohmann::json;
    const auto& last = request.messages.back();
    const auto call = [&](const std::string& skill, const Json& arguments) -> Response {
        const auto tool = std::ranges::find_if(request.tools, [&](const auto& def) { return def.name.ends_with("_" + skill); });
        if (tool == request.tools.end()) throw std::runtime_error("missing registered skill: " + skill);
        return {{Role::assistant, "", {{skill, tool->name, arguments.dump()}}}, FinishReason::tool_calls};
    };
    if (request.tools.size() != 4 || request.messages.front().role != Role::system ||
        std::ranges::count_if(request.messages, [](const auto& msg) { return msg.role == Role::system; }) != 1)
        throw std::runtime_error("wrong partner guidance or skill scope");
    if (last.role == Role::user) {
        const auto input = Json::parse(last.content);
        const auto& raw = input.at("messages").back().at("text");
        if (raw == "大家叫我小叶") return {{Role::assistant, "小叶，我记住了。"}};
        if (raw == "这回带我往前走走") return call("move", {{"x",32},{"y",-60},{"z",-4}});
        if (raw == "改主意了，先站这儿" && input.value("interruptedRequest", "") == "这回带我往前走走")
            return {{Role::assistant, "好，我停在这儿。"}};
        if (raw == "模拟一次服务故障") throw std::runtime_error("fixture provider unavailable");
        if (raw != "大肥鱼，帮我采俩泥土" || !std::ranges::any_of(request.messages, [](const auto& m) {
            return m.role == Role::assistant && m.content == "小叶，我记住了。";
        })) throw std::runtime_error("missing original or previous exchange");
        return call("observe", {{"block","dirt"}});
    }
    const auto result = Json::parse(last.content.substr(last.content.find('{')));
    if (result.at("status") != "done") throw std::runtime_error("skill blocked: " + result.dump());
    if (last.tool_call_id == "observe" && !result.at("visible").empty())
        return call("move", {{"x",1},{"y",-60},{"z",0}});
    if (last.tool_call_id == "move" && result.contains("position"))
        return call("collect", {{"block","minecraft:dirt"},{"count",2}});
    if (last.tool_call_id == "collect" && result.at("broken") == 2 && result.at("inventoryDelta").at("dirt") == 2)
        return call("reply", {{"text","小叶，背包里多了两块泥土。"}});
    if (last.tool_call_id == "reply" && result.at("sent") == true) return {{Role::assistant, "小叶，背包里多了两块泥土。"}};
    throw std::runtime_error("unexpected game receipt");
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
            [partner](const auto&, const auto& request) { return partner ? partner_sample(request) : sample_turn(request); }});
        app::run_companion_host(host, argv[1], root, partner ? app::CompanionMode::partner : app::CompanionMode::scenario);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
