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
    std::vector<std::vector<ToolDefinition>> projections;
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
    options.compare_projection = true;
    options.model_capture = [&](const ModelCapture& capture) {
        if (capture.phase == ModelCapturePhase::prepared && capture.projected_tools && capture.evidence_revision) {
            prepared.push_back(*capture.input);
            projections.push_back(*capture.projected_tools);
        }
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
    check(projections[0].size() == 2 && projections[2].size() == 2 &&
        projections[0][0].name == projections[2][0].name && projections[0][1].name == projections[2][1].name &&
        projections[3].empty() && projections[5].empty(), "projection changed grants or followed caller order");
    check(prepared[3].tools.empty() && prepared[5].tools.empty() && contains(prepared[5],"use a chest"),"revision or planning authority changed");
    check(contains(prepared[2],"inventory-receipt-8") && contains(prepared[2],"raw-A"),"ordinary baseline lost the receipt or raw reply");
    const bool same = prepared[0].tools.front().name == prepared[2].tools.front().name;
    check(same && contains(prepared[3],"stock-W17") && contains(prepared[3],"inventory-receipt-8") &&
        contains(prepared[5],"inventory-receipt-8") && contains(prepared[5],"eight in bag") &&
        contains(prepared[5],"deliver eight"),"plan projection lost original receipts, new guidance or verified prefix");
    std::cout << "stable_tool_order=" << same << " ordinary_has_raw_A=" << contains(prepared[2],"raw-A")
              << " ordinary_has_effective_B=" << contains(prepared[2],"effective-B")
              << " plan_has_inventory=" << contains(prepared[3],"inventory-receipt-8")
              << " revision_has_inventory=" << contains(prepared[5],"inventory-receipt-8")
              << " prepared=" << prepared.size() << " dispatches=" << dispatches << '\n';
}

// 连续提交问题基线、关闭增量与同版引用，拒收替换及活动中断都不提前消费状态。
// 步骤：冻结同版投影 → 在会话内关闭问题 → 拒绝非法替换 → 从原历史继续并检查单次关闭。
void status_versions() {
    ContextStatus open{"player/world",{{"Q4","waiting for chest"},{"W17","mining"}}};
    ContextStatus closed{"player/world",{{"W17","mining"}}};
    auto delta = project_context_status(&open,closed);
    check(delta == project_context_status(&open,closed) && delta.find(R"("closed":["Q4"])") != delta.npos,
        "status projection consumed a change during comparison");
    std::vector<Request> prepared;
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({},channel,Backend{[](const Config&,const Request&) { return Response{{Role::assistant,"ok"}}; }});
    RunOptions options;
    options.model_capture = [&](const ModelCapture& capture) {
        if (capture.phase == ModelCapturePhase::prepared) prepared.push_back(*capture.input);
    };
    options.context_status = std::make_shared<const ContextStatus>(open);
    (void)session.begin("original",options);
    until(session,[&] { return !session.busy(); });
    options.context_status = std::make_shared<const ContextStatus>(closed);
    (void)session.begin("close Q4",options);
    until(session,[&] { return !session.busy(); });
    RunOptions invalid = options;
    invalid.replace_history = true;
    invalid.context_status = std::make_shared<const ContextStatus>(ContextStatus{});
    bool rejected = false;
    try { (void)session.begin("new baseline",invalid); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected && !session.busy() && prepared.size() == 2,"invalid baseline modified admission");
    (void)session.begin("continue",options);
    until(session,[&] { return !session.busy(); });
    const auto& tail = prepared.back().messages;
    check(contains(prepared[0],R"("mode":"baseline")") && contains(prepared[1],delta) &&
        tail[tail.size()-2].content.find(R"("mode":"reference")") != std::string::npos &&
        contains(prepared.back(),"original"),"replacement failure lost history or status version");
}

// 建立十六个真实候选，分别验证整批拒绝、有界回执可执行和模型窗口不足。
// 步骤：声明可见回执上限 → 提交十六个固定工具调用 → 核对分派数、返回配对及原号证据。
void batch_capacity(std::uint32_t view, std::uint32_t window, bool succeeds) {
    std::atomic<unsigned> calls = 0;
    unsigned actions = 0;
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Config config;
    config.context_window_tokens = window;
    config.reserved_output_tokens = window ? 512 : 0;
    Session session(config,channel,Backend{[&](const Config&,const Request& input) -> Response {
        if (++calls == 1) {
            std::vector<ToolCall> batch;
            for (unsigned n = 0; n < 16; ++n) batch.push_back({"r"+std::to_string(n),"read","{}"});
            return {{Role::assistant,"",std::move(batch)},FinishReason::tool_calls};
        }
        check(input.messages.size() == 18 && input.messages.back().tool_call_id == "r15" &&
            input.messages.back().content.find("host_evidence") != std::string::npos,"result preview lost original pair");
        return {{Role::assistant,"finished"}};
    }});
    ToolCatalog catalog;
    ToolDescription desc{"test/read",1,1,{"read","read",R"({"type":"object","properties":{},"additionalProperties":false})"}};
    desc.receipt_view_bytes = view;
    catalog.add(desc,[&](const auto&) {
        ++actions;
        return completed_tool({true,ToolEffect::not_applied,std::string(5000,'r'),ToolError::none});
    });
    RunOptions options;
    const std::vector<std::string> ids{desc.id};
    options.tools = catalog.snapshot(ids);
    const auto turn = session.begin("all or none",options);
    until(session,[&] { return !session.busy(); });
    if (!succeeds) {
        check(actions == 0 && calls == 1 && turn->operation()->state() == OperationState::failed,"capacity check allowed a partial dispatch");
        return;
    }
    check(turn->answer() == "finished" && actions == 16 && calls == 2,"bounded batch did not execute once");
    const auto epoch = session.evidence_epoch();
    const auto full = session.read_evidence(epoch,18,1);
    check(full.front().tool_call_id == "r15" && full.front().content.ends_with(std::string(5000,'r')),"original receipt not recoverable");
    session.clear();
    bool stale = false;
    try { session.read_evidence(epoch,18,1); } catch (const std::invalid_argument&) { stale = true; }
    check(stale,"old evidence survived a scope change");
}

// 验证容量压力只压缩已结算的整批，并保留原话、未知效果、取消及其工具配对。
void compress_settled() {
    Request input{{{Role::user,"U37 original goal"}}};
    for (unsigned i = 0; i < 6; ++i) {
        const auto id = "work"+std::to_string(i);
        input.messages.push_back({Role::assistant,"",{{id,"read","{}"}}});
        Message result{Role::tool,std::string(1500,'v'),{},id};
        result.settled_evidence = i != 2;
        if (i == 2) result.content = "cancelled; pickup unknown for W17";
        input.messages.push_back(std::move(result));
    }
    input.messages.push_back({Role::user,"Q4 waiting"});
    const auto compressed = fit_context_request(input,{6200,512,73,false});
    check(compressed && input.max_output_tokens == 512 && contains(input,"U37 original goal") && contains(input,"Q4 waiting") &&
        contains(input,"pickup unknown for W17") && contains(input,"settled_tool_batch"),"compaction lost protected work or original reference");
    validate_request(input);
}
}

// 运行固定响应场景并把实际边界差异交给离线测试。
int main() {
    try {
        replay(); status_versions(); compress_settled();
        batch_capacity(0,0,false); batch_capacity(768,0,true); batch_capacity(768,4096,false);
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
