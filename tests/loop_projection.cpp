#include <dafeiyu/session.hpp>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::model;
using namespace dfy::runtime;

// 报告实际送模、工具分派或回合状态与固定场景的差异。
void check(bool value, const char* reason) {
    if (!value) throw std::runtime_error(reason);
}

// 在同一会话推进原工作，直至目标状态出现或限时退出。
void until(Session& session, const auto& ready) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (!ready() && std::chrono::steady_clock::now() < limit) {
        session.advance();
        std::this_thread::yield();
    }
    check(ready(), "session did not reach the requested boundary");
}

// 查找冻结请求内任一消息中出现的事实正文。
bool contains(const Request& request, std::string_view needle) {
    return std::ranges::any_of(request.messages, [&](const Message& message) {
        return message.content.find(needle) != std::string::npos;
    });
}

// 提供声明式两步计划，前缀验证和修订仍由正式宿主处理。
Response proposal() {
    return {{Role::assistant, R"({"schema_version":1,"steps":[{"id":"count","objective":"check","completion_criterion":"checked","tools":[]},{"id":"deliver","objective":"deliver","completion_criterion":"received","tools":[]}]})"}};
}

// 从实际模型入口冻结授权、库存和回复样本，并对照同一条计划修订链。
// 步骤：取得工具实效并改写回复 → 交换同组授权提交新原话 → 执行规划、首步与修订 → 核对请求数和实际分派。
void replay() {
    std::vector<Request> prepared;
    std::atomic<unsigned> attempts = 0;
    unsigned dispatches = 0;
    Backend fixed{[&](const Config&, const Request&) -> Response {
        switch (++attempts) {
        case 1: return {{Role::assistant, "", {{"stock-W17","inventory","{}"}}}, FinishReason::tool_calls};
        case 2: return {{Role::assistant, "raw-A"}};
        case 4: case 6: return proposal();
        default: return {{Role::assistant, "observed"}};
        }
    }};
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({}, channel, fixed);
    const auto factory = [&](const auto&) {
        ++dispatches;
        return completed_tool({true, ToolEffect::not_applied, "inventory-receipt-8", ToolError::none});
    };
    ToolCatalog catalog;
    for (const std::string name : {"inventory","inspect"}) {
        catalog.add({"test/"+name, 1, 1, {name,"read",R"({"type":"object","properties":{},"additionalProperties":false})"}}, factory);
    }
    const std::vector<std::string> first{"test/inventory","test/inspect"}, swapped{"test/inspect","test/inventory"};
    RunOptions options;
    options.model_capture = [&](const ModelCapture& capture) {
        if (capture.phase == ModelCapturePhase::prepared) prepared.push_back(*capture.input);
    };
    options.text_hook = [](TextHookPoint point, const auto&, auto, auto text, auto) -> std::optional<std::string> {
        if (point == TextHookPoint::reply && text == "raw-A") return "effective-B";
        return {};
    };
    options.tools = catalog.snapshot(first);
    const auto counted = session.begin("count stock", options);
    until(session,[&] { return !session.busy(); });
    check(counted->answer() == "effective-B", "reply hook did not produce B");
    options.tools = catalog.snapshot(swapped);
    (void)session.begin("report stock",options);
    until(session,[&] { return !session.busy(); });

    ConfirmationCaller caller;
    options.planning = PlanLimits{16,2,16*1024};
    options.confirmation_caller = caller;
    const auto planning = session.begin("deliver eight",options);
    until(session,[&] { return planning->plan()->definition && planning->plan()->steps[0].waiting == PlanWait::verification; });
    auto state = *planning->plan();
    session.confirm_plan_step(caller,state.steps[0].key,"eight in bag");
    session.replan(caller,state.definition->binding(),"use a chest");
    until(session,[&] { return planning->plan()->steps[1].waiting == PlanWait::verification; });
    session.confirm_plan_step(caller,planning->plan()->steps[1].key,"received");
    until(session,[&] { return !session.busy(); });

    check(attempts == 7 && prepared.size() == 7 && dispatches == 1, "unexpected external work");
    check(prepared[3].tools.empty() && prepared[5].tools.empty() && contains(prepared[5],"use a chest"),"revision or planning authority changed");
    check(contains(prepared[2],"inventory-receipt-8") && contains(prepared[2],"raw-A"),"ordinary baseline lost the receipt or raw reply");
    const bool same = prepared[0].tools.front().name == prepared[2].tools.front().name;
    std::cout << "stable_tool_order=" << same << " ordinary_has_raw_A=" << contains(prepared[2],"raw-A")
              << " ordinary_has_effective_B=" << contains(prepared[2],"effective-B")
              << " plan_has_inventory=" << contains(prepared[3],"inventory-receipt-8")
              << " revision_has_inventory=" << contains(prepared[5],"inventory-receipt-8")
              << " prepared=" << prepared.size() << " dispatches=" << dispatches << '\n';
}
}

// 运行固定响应场景并把实际边界差异交给离线测试。
int main() {
    try { replay(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
