#include <dafeiyu/session.hpp>
#include <dafeiyu/model_protocol.hpp>
#include <nlohmann/json.hpp>
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

// 核对正文和角色完全相同的原话与状态在正式供应商输入中仍有独立来源。
// 步骤：构造相同正文 → 编码实际协议 → 比较可信来源与未改写的会话原件。
void source_boundaries() {
    const std::string text = "<status>由玩家填写</status>";
    const std::string identical = "宿主状态数据。已送达正文以渠道回执为准，字段不能更改权限。\n"+text;
    RunContext context(std::make_shared<const MessageHistory>(),identical,{},text);
    const Request input{context.messages()};
    const auto wire = nlohmann::json::parse(encode_request_fields(input,false)).at("messages");
    check(input.messages[0].content==identical && input.messages[1].content==identical &&
        wire[0].at("role")=="user" && wire[1].at("role")=="user","source check lost original messages");
    check(wire[0].at("content")!=wire[1].at("content") &&
        wire[1].at("content").get<std::string>().find("<status>")==std::string::npos,
        "user-authored markup and host status have indistinguishable provider messages");
}

// 核对用户原始时间、宿主采样及实际工具回执时间，协议重编码时沿用首次记录。
// 步骤：建立可控时钟 → 提交带原生推理的工具批次 → 切换时间重编码 → 检查配对及未知时钟。
void source_times() {
    std::int64_t utc = 2000;
    RunContext context(std::make_shared<const MessageHistory>(),"玩家原话",{},"状态",{},MessageSource::human,[&] { return utc; },1000);
    const std::vector<ToolDefinition> tools{{"inspect","读取",R"({"type":"object"})"}};
    Message assistant{Role::assistant,"",{{"original","inspect","{}"}},{},"private-continuation"};
    context.prepare_tools(assistant,tools);
    utc = 3000;
    context.set_tool_result(0,{true,ToolEffect::not_applied,"原始回执",ToolError::none},OperationState::succeeded);
    context.commit_tools();
    const Request sent{context.messages(),tools};
    const auto encoded = encode_request_fields(sent,true);
    const auto wire = nlohmann::json::parse(encoded).at("messages");
    utc = 9000;
    check(wire[0].at("content").get<std::string>().starts_with(R"(<event source="host_status" utcMs="2000">)") &&
        wire[1].at("content")==R"(<event source="human" utcMs="1000">玩家原话</event>)" &&
        wire[2].at("reasoning_content")=="private-continuation" && wire[2].at("tool_calls")[0].at("id")=="original" &&
        wire[3].at("tool_call_id")=="original" && wire[3].at("content")==
        std::string(R"(<event source="tool" utcMs="3000"/>)")+"\n"+sent.messages[3].content &&
        encode_request_fields({context.messages(),tools},true)==encoded,"event times or native tool pairing changed on repeat");
    RunContext unknown(std::make_shared<const MessageHistory>(),"保持原话",{},"",{},MessageSource::human,
        []() -> std::int64_t { throw std::runtime_error("clock unavailable"); });
    check(!unknown.messages().back().event_utc_ms && wire_content(unknown.messages().back())==
        R"(<event source="human">保持原话</event>)","unavailable clock invented an event time");
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
    options.decision_status = DecisionStatusOptions{"projection",{},false};
    options.prompt_rules = PromptRules{OutputRoute::main_chat_with_reply,"固定语气"};
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
    ObservationTime now{};
    ObservationSource source(17,ObservationTrust::external_data);
    ObservationInbox inbox({source.id()});
    const FieldDescriptor fields[] = {{"stock",ValueKind::uint64,0}};
    auto descriptor = std::make_shared<const StructDescriptor>(StructDescriptor::create("fixture.stock",1,fields,{1,64}));
    const NamedValue values[] = {{"stock",Value::from_uint64(8)}};
    auto observed = std::make_shared<const StructValue>(StructValue::create(descriptor,values,64));
    for (const std::string id : {"backpack","ground"}) {
        const auto lifetime = std::chrono::milliseconds(id=="backpack" ? 1000 : 50);
        inbox.accept(source.capture({id,2},1,now,now+lifetime,observed),now);
        options.observations.push_back(inbox.latest(source.id(),id));
    }
    options.now = [&] { return now; };
    options.message_clock = [&] { return 1250+std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count(); };
    options.planning = PlanLimits{16,2,16*1024};
    options.prompt_utc_ms = 1250;
    options.confirmation_caller = caller;
    const auto planning = session.begin("deliver eight",options);
    until(session,[&] { return planning->plan()->definition && planning->plan()->steps[0].waiting == PlanWait::verification; });
    auto state = *planning->plan();
    now += std::chrono::milliseconds(20);
    session.confirm_plan_step(caller,state.steps[0].key,"eight in bag");
    now += std::chrono::milliseconds(80);
    session.replan(caller,state.definition->binding(),"use a chest");
    until(session,[&] { return planning->plan()->steps[1].waiting == PlanWait::verification; });
    session.confirm_plan_step(caller,planning->plan()->steps[1].key,"received");
    until(session,[&] { return !session.busy(); });

    check(prepared[0].messages.front().content.find("main_reply")!=std::string::npos &&
        prepared[3].messages.front().content.find("将计划提议交给宿主审查")!=std::string::npos &&
        prepared[3].messages.front().content.find("main_reply")==std::string::npos &&
        prepared[4].messages.front().content.find("本步结果交给宿主核验")!=std::string::npos,
        "planning or step settlement inherited the wrong delivery route");
    check(attempts == 7 && prepared.size() == 7 && dispatches == 1, "unexpected external work");
    for (std::size_t i=0; i<prepared.size(); ++i) {
        const auto& messages = prepared[i].messages;
        const auto tail = nlohmann::json::parse(messages.back().content);
        check(messages.back().role==Role::user && messages.back().source==MessageSource::host_status &&
            std::ranges::count(messages,MessageSource::host_status,&Message::source)==1 &&
            tail.at("schema")=="dafeiyu.agent_status/1" && tail.at("items").size()==(i>=4 ? 1u : 0u) &&
            tail.at("step")==std::array{1,2,1,1,2,3,4}[i], "unexpected or accumulated decision tail");
        const auto& scope = tail.at("scope");
        if (i>=3) check(scope.at("task")=="plan/"+scope.at("session").get<std::string>()+"/"+
            std::to_string(tail.at("turn").get<std::uint64_t>()),"replanning lost its original task");
        else check(!scope.contains("task"),"ordinary conversation acquired an unrelated plan");
        if (i==1) check(messages[messages.size()-2].tool_call_id=="stock-W17","tail split tool continuation");
        if (i>=4) {
            const auto& todo = tail.at("items").at(0);
            const auto& value = todo.at("value");
            check(todo.at("key")=="host.plan_todo" && todo.at("attention").at("tasks").at(0)==scope.at("task") &&
                value.at("id")==(i==4 ? "count" : "deliver") && value.at("state")==(i==5 ? "pending" : "running") &&
                value.at("createdUtcMs")==1250 && value.at("planRevision")==(i==6 ? 2 : 1),"wrong active TODO or original creation");
            if (i==6) {
                const auto original = nlohmann::json::parse(session.read_status(std::stoull(scope.at("epoch").get<std::string>()),
                    tail.at("snapshot").get<std::string>())).at("status").at("items");
                const auto& prefix = original.at(0).at("value");
                check(original.size()==2 && prefix.at("id")=="count" && prefix.at("state")=="completed" && prefix.at("evidence")=="eight in bag" &&
                    prefix.at("originPlanRevision")==1 && prefix.at("updatedUtcMs")==1270 && value.at("updatedUtcMs")==1350,
                    "replan lost the verified prefix or refreshed its timestamp");
            }
        }
    }
    check(projections[0].size() == 2 && projections[2].size() == 2 &&
        projections[0][0].name == projections[2][0].name && projections[0][1].name == projections[2][1].name &&
        projections[3].empty() && projections[5].empty(), "projection changed grants or followed caller order");
    check(prepared[3].tools.empty() && prepared[5].tools.empty() && contains(prepared[5],"use a chest"),"revision or planning authority changed");
    const auto first_goal = nlohmann::json::parse(prepared[3].messages.at(prepared[3].messages.size()-2).content).at("host_evidence").at("goal_message");
    const auto revised_goal = nlohmann::json::parse(prepared[5].messages.at(prepared[5].messages.size()-2).content).at("host_evidence").at("goal_message");
    check(first_goal==revised_goal && first_goal.at("eventUtcMs")==1250 && first_goal.at("source")=="human" &&
        session.read_evidence(session.evidence_epoch(),first_goal.at("message").get<std::size_t>(),1).front().content=="deliver eight",
        "replanning lost the original goal event in retained dialogue");
    const auto first_samples = nlohmann::json::parse(prepared[3].messages.at(prepared[3].messages.size()-2).content).at("host_evidence").at("observations");
    const auto revised_samples = nlohmann::json::parse(prepared[5].messages.at(prepared[5].messages.size()-2).content).at("host_evidence").at("observations");
    check(first_samples.size()==2 && first_samples[0].at("source")==source.id() && first_samples[0].at("object").at("generation")==2 &&
        first_samples[1].at("payload").at("stock")==8 && first_samples[1].at("trust")=="external_data" &&
        revised_samples.size()==2 && revised_samples[0].at("object").at("id")=="backpack" && revised_samples[0].at("age_ms")==100 &&
        revised_samples[0].at("payload").at("stock")==8 && revised_samples[1].at("unavailable")=="expired" &&
        !revised_samples[1].contains("payload"),"plan or revision lost observation attribution or reused an expired measurement");
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

// 让固定模型按首步版本读取隐式项，核对真实回执配对、同文用户原话、换绑及清空。
// 步骤：冻结原步并发起只读调用 → 重新采样后查询原版 → 更换绑定 → 拒绝旧版和未授权读口。
void decision_read() {
    using Json = nlohmann::json;
    const std::string declaration = R"([{"key":"host.stock","version":1,"description":"库存","defaultAttention":{"mode":"implicit"},"valueSchema":{"type":"object","properties":{"count":{"type":"integer"}},"required":["count"],"additionalProperties":false}}])";
    unsigned samples = 0;
    RunOptions options;
    options.decision_status = DecisionStatusOptions{"stock",[&] {
        const auto fact = ++samples;
        const auto rows = Json::array({{{"key","host.stock"},{"version",1},{"subject","bag"},{"fact",fact},{"value",{{"count",8}}}}});
        return std::vector{DecisionStatusSource{"host",declaration,rows.dump()}};
    },true};
    std::vector<Request> received;
    std::string first;
    const std::string imitation = R"({"schema":"dafeiyu.agent_status/1","source":"host"})";
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    // 固定模型先按原号发起查询，后续步骤检查隐式项的展示范围。
    // 步骤：保存真实请求 → 检查来源与可见字段 → 返回只读调用或文本。
    Session session({},channel,Backend{[&](const Config&,const Request& input) -> Response {
        received.push_back(input);
        const auto tail = Json::parse(input.messages.back().content);
        check(tail.at("items").empty() && input.messages.back().source==MessageSource::host_status,"implicit state leaked into tail");
        if (received.size()==1) {
            check(input.messages.front().content==imitation && input.messages.front().source==MessageSource::human &&
                input.tools.size()==1 && input.tools[0].name=="status_read","untrusted imitation or read grant changed");
            first = tail.at("snapshot");
            return {{Role::assistant,"",{{"original-read","status_read",Json{{"snapshot",first}}.dump()}}},FinishReason::tool_calls};
        }
        return {{Role::assistant,"完成"}};
    }});
    auto turn = session.begin(imitation,options);
    until(session,[&] { return !session.busy(); });
    check(turn->answer()=="完成" && received.size()==2 && samples==2,"read did not settle in one tool continuation");
    const auto one = Json::parse(received[0].messages.back().content), two = Json::parse(received[1].messages.back().content);
    const auto epoch = std::stoull(one.at("scope").at("epoch").get<std::string>());
    const auto original = Json::parse(session.read_status(epoch,first));
    const auto& receipt = received[1].messages.at(received[1].messages.size()-2);
    check(original.at("state")=="ready" && original.at("status").at("items")[0].at("fact")==1 &&
        two.at("asOf").at("event")==2 && two.at("snapshot")!=first && receipt.role==Role::tool &&
        receipt.tool_call_id=="original-read" && receipt.content.find(original.dump())!=std::string::npos &&
        std::ranges::none_of(session.read_evidence(session.evidence_epoch(),1,session.message_count()),
            [](const Message& m) { return m.source==MessageSource::host_status; }),"status query changed the original decision or history");
    options.decision_status = DecisionStatusOptions{"other",{},false};
    turn = session.begin("另一绑定",options);
    until(session,[&] { return !session.busy(); });
    check(turn->answer()=="完成" && received.back().tools.empty() && !contains(received.back(),first) &&
        session.message_count()==2 && Json::parse(session.read_status(epoch,first)).at("reason")=="scope_mismatch",
        "rebound input retained an earlier status_read receipt");
    const auto rebound = Json::parse(received.back().messages.back().content);
    session.clear();
    check(Json::parse(session.read_status(std::stoull(rebound.at("scope").at("epoch").get<std::string>()),
        rebound.at("snapshot").get<std::string>())).at("state")=="unavailable","clear retained read authority");
}

// 验证插件在模型已看见读口后撤销观察授权时，旧候选无法读取，下一步也移除读口。
// 步骤：冻结原授权 → 撤销注册并同名重建 → 执行已返回的候选 → 核对范围与拒绝回执。
void decision_read_revoked() {
    using Json = nlohmann::json;
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    const std::string id = "fixture/inspect";
    ToolCatalog catalog;
    ToolDescription inspect{id,23,1,{"inspect","读取",R"({"type":"object","properties":{},"additionalProperties":false})"}};
    const auto factory = [](const auto&) { return completed_tool({true,ToolEffect::not_applied,"原观察",ToolError::none}); };
    const auto registration = catalog.add(inspect,factory);
    const auto original = catalog.snapshot(std::span(&id,1));
    RunOptions options;
    options.tools = original.bind_context(R"({"player":"original"})");
    options.decision_status = DecisionStatusOptions{"fixture",{},true,original};
    unsigned calls = 0;
    // 固定模型按旧工具表提出候选，随后验证原注册撤销的回执。
    // 步骤：读取首步原快照号 → 提交候选 → 检查下一步范围和拒绝原因。
    Session session({},channel,Backend{[&](const Config&,const Request& request) -> Response {
        if (++calls==1) {
            check(request.tools.size()==2 && request.tools.back().name=="status_read","missing dependent status reader");
            const auto tail = Json::parse(request.messages.back().content);
            return {{Role::assistant,"",{{"late-read","status_read",Json{{"snapshot",tail.at("snapshot")}}.dump()}}},FinishReason::tool_calls};
        }
        const auto& receipt = request.messages.at(request.messages.size()-2);
        check(request.tools.empty() && receipt.role==Role::tool && receipt.tool_call_id=="late-read" &&
            receipt.content.find("revoked")!=std::string::npos && receipt.content.find(R"("state":"ready")")==std::string::npos,
            "superseded observation grant exposed old status");
        return {{Role::assistant,"原授权已撤回"}};
    }});
    auto turn = session.begin("查当前状态",options);
    catalog.remove(registration);
    catalog.add(inspect,factory);
    until(session,[&] { return !session.busy(); });
    check(calls==2 && turn->answer()=="原授权已撤回","revoked reader did not settle with its original call");
}

// 第一次模型传输失败后保留原事件，下一轮的有效工具调用只消费实际展示的一次。
// 步骤：记录失败请求 → 按原关注再次送模 → 返回快照读工具 → 核对原版及续行尾注。
void counted_responses() {
    using Json = nlohmann::json;
    DecisionStatusSource source{"host",
        R"([{"key":"host.signal","version":1,"description":"工作完成","defaultAttention":{"mode":"implicit"},"valueSchema":{"type":"object","properties":{},"additionalProperties":false}}])",
        R"([{"key":"host.signal","version":1,"subject":"work","fact":9,"value":{}}])",
        R"([{"key":"host.signal","version":1,"subject":"work","fact":9,"mode":"counted","count":1,"event":42}])"};
    RunOptions options;
    options.decision_status = DecisionStatusOptions{"binding",[source] { return std::vector{source}; },true};
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    unsigned calls = 0;
    // 固定传输先失败一次，再使用原号查询，最后从配对工具回执确认该决策的旧视图。
    // 步骤：核对此步来源 → 注入失败或返回查询调用 → 比对已消费尾注与原决策。
    Session session({},channel,Backend{[&](const Config&,const Request& input) -> Response {
        const auto tail = Json::parse(input.messages.back().content);
        if (++calls<3) {
            check(tail.at("wakeEvents")==Json::array({42}) && tail.at("items").size()==1,"failed request consumed counted attention");
            if (calls==1) throw std::runtime_error("fixed transport failed");
            return {{Role::assistant,"",{{"old","status_read",Json{{"snapshot",tail.at("snapshot")}}.dump()}}},FinishReason::tool_calls};
        }
        const auto& reply = input.messages[input.messages.size()-2];
        check(tail.at("wakeEvents").empty() && tail.at("items").empty() && reply.tool_call_id=="old" &&
            Json::parse(reply.content.substr(reply.content.find('{'))).at("status").at("wakeEvents")==Json::array({42}),
            "accepted tool response failed to settle the original view");
        return {{Role::assistant,"已读回原事件"}};
    }});
    auto failed = session.begin("检查回执",options);
    until(session,[&] { return !session.busy(); });
    check(failed->operation()->state()==OperationState::failed,"fixed failure was not observed");
    auto retried = session.begin("再查原回执",options);
    until(session,[&] { return !session.busy(); });
    check(calls==3 && retried->answer()=="已读回原事件","retry or tool continuation did not settle");
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

// 在同一历史中比较近期摘要、容量充足和摘要失效三条路径。
// 步骤：构造可归档旧对话 → 复核摘要窗口 → 对照原件、当前目标及回退请求。
void traceable_recent_dialogue() {
    Request original{{{Role::user,"最初目标"},{Role::assistant,"已接纳"}}};
    auto old = sourced_message(MessageSource::human,"买牛奶排了二十分钟");
    old.source_message = 3;
    original.messages.push_back(std::move(old));
    original.messages.push_back({Role::assistant,std::string(4800,'x')});
    original.messages.back().completed_dialogue_span = 2;
    original.messages.push_back({Role::user,"这句收一点"});
    original.messages.push_back({Role::assistant,"我明白了"});
    original.messages.push_back({Role::user,"现在先确认还没核实的事项"});
    original.messages.push_back(sourced_message(MessageSource::host_status,"W17 的实际效果未知"));
    constexpr ContextWindow small{2000,256,56,false};
    auto baseline = original;
    fit_context_request(baseline,small);
    unsigned calls = 0;
    const DialogueSummary summarize = [&](std::span<const Message> dialogue) -> std::optional<std::string> {
        ++calls;
        check(dialogue.size()==2 && dialogue.front().source_message==3 && dialogue.back().content.size()==4800,
            "recent summary received unsettled material or lost its original identity");
        return R"({"source":"recent_dialogue","reference":"profile/3","topic":"买牛奶时排队"})";
    };
    auto result = original;
    fit_context_request(result,small,0,summarize);
    check(calls==1 && result.messages[2].source==MessageSource::host_summary && contains(result,"profile/3") &&
        contains(result,"这句收一点") && contains(result,"现在先确认还没核实的事项") && contains(result,"W17 的实际效果未知") &&
        encode_request_fields(result,false).size()<encode_request_fields(baseline,false).size(),"recent summary did not preserve protected material");
    auto roomy = original;
    fit_context_request(roomy,{12000,256,56,false},0,summarize);
    check(calls==1 && roomy.messages[3].content.size()==4800,"summary ran while the original request fit");
    auto fallback = original;
    fit_context_request(fallback,small,0,[](auto) { return std::string(2049,'x'); });
    check(encode_request_fields(fallback,false)==encode_request_fields(baseline,false) && original.messages[3].content.size()==4800,
        "invalid summary modified the archive or the fallback");
}

// 在普通与带工具的思考会话中越过 512 轮，核对实际送模的历史归属和按位置读回的原件。
// 步骤：连续提交固定问答 → 冻结最后一轮请求 → 对照每组原话、回答、角色及保护边界。
void long_dialogue(bool thinking) {
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Config config; config.provider = "deepseek"; config.thinking = thinking;
    ToolCatalog catalog;
    RunOptions options;
    if (thinking) {
        const std::string id = "test/read";
        catalog.add({id,1,1,{"read","read",R"({"type":"object","properties":{},"additionalProperties":false})"}},[](const auto&) {
            return completed_tool({true,ToolEffect::not_applied,"fixed",ToolError::none});
        });
        options.tools = catalog.snapshot(std::span(&id,1));
    }
    unsigned calls = 0;
    Request last;
    Session session(config,channel,Backend{[&](const Config&,const Request& input) {
        ++calls;
        if (calls == 515) last = input;
        return Response{{Role::assistant,"reply-"+std::to_string(calls),{},{},"private-reasoning"}};
    }});
    for (unsigned i = 1; i <= 515; ++i) {
        const auto turn = session.begin("prompt-"+std::to_string(i),options);
        until(session,[&] { return !session.busy(); });
        check(turn->answer() == "reply-"+std::to_string(i),"long dialogue lost its current answer");
    }
    check(calls == 515 && session.message_count() == 1030 && last.messages.size() <= 1024 && last.tools.size()==(thinking ? 1u : 0u) &&
        last.messages.front().content == "prompt-1" && last.messages.back().content == "prompt-515",
        "long dialogue lost the first or current request");
    if (thinking) {
        const auto messages = nlohmann::json::parse(encode_request_fields(last,true)).at("messages");
        check(messages[1].at("reasoning_content")=="private-reasoning" &&
            messages[messages.size()-2].at("reasoning_content")=="private-reasoning","protected native reasoning was lost");
    }
    unsigned packed = 0;
    for (const auto& message : last.messages) {
        if (message.content.find("completed_dialogue") == std::string::npos) continue;
        const auto ref = nlohmann::json::parse(message.content);
        const auto original = session.read_evidence(ref.at("epoch"),ref.at("first"),ref.at("count"));
        check(message.role == Role::user && message.content.find("private-reasoning") == std::string::npos &&
            original.size() == 2*ref.at("turns").size(),"packed dialogue lost source or exposed private reasoning");
        std::size_t index = 0;
        for (const auto& turn : ref.at("turns")) {
            check(turn.at("user") == original[index].content && turn.at("assistant") == original[index+1].content &&
                original[index+1].reasoning_content == "private-reasoning","packed dialogue differs from the original");
            index += 2; ++packed;
        }
    }
    check(packed > 0,"long dialogue did not use attributed history");
}

// 核对带状态的问答位置，保护未决工具和需要由思考协议独立传递的消息。
// 步骤：加入实际系统段与已结算问答 → 触发消息数投影 → 查验角色、原始位置及未决结果。
void mixed_dialogue(bool all_reasoning) {
    Request input{{{Role::system,"injected head"},{Role::user,"original goal"},{Role::assistant,"original answer"}}};
    input.tools.push_back({"read","read",R"({"type":"object"})"});
    for (unsigned i = 0; i < 344; ++i) {
        input.messages.push_back({Role::user,"status reference"});
        input.messages.push_back({Role::user,"follow-up"});
        Message answer{Role::assistant,"answer"};
        answer.completed_dialogue_span = 3;
        if (all_reasoning || i==1) answer.reasoning_content = "protocol-only";
        input.messages.push_back(std::move(answer));
    }
    input.messages.push_back({Role::assistant,"",{{"pending","read","{}"}},{},"original tool continuation"});
    input.messages.push_back({Role::tool,"effect unknown",{},"pending"});
    input.messages.push_back({Role::user,"current revision"});
    fit_context_request(input,{0,0,73,true},1);
    const auto group = nlohmann::json::parse(input.messages[3].content);
    check(group.at("first")==3 && group.at("turns").at(0).at("host_status")=="status reference" &&
        group.dump().find("protocol-only")==std::string::npos && input.messages.back().content=="current revision" &&
        input.messages[input.messages.size()-2].tool_call_id=="pending" &&
        input.messages[input.messages.size()-3].reasoning_content=="original tool continuation",
        "mixed projection lost attribution, privacy or pending tool continuation");
    if (!all_reasoning) check(group.at("count")==3 && input.messages[6].reasoning_content=="protocol-only",
        "native ordinary reasoning was folded despite sufficient space");
    validate_request(input);
}

// 分别以协议字节和配置窗口触发压缩，对照历史原话、长回答两端及原文读取。
// 步骤：逐轮提交可识别的长回答 → 捕获受压后的实际请求 → 按原始位置核对保留和省略的正文。
void long_answers(unsigned turns, unsigned blocks, std::uint32_t window) {
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Config config;
    config.context_window_tokens = window; config.reserved_output_tokens = window ? 512 : 0;
    std::string body;
    for (unsigned n=0;n<blocks;++n) body += "长文";
    body.insert(body.size()/2,"original-middle");
    body.insert(0,"#");
    unsigned calls = 0;
    Request last;
    Session session(config,channel,Backend{[&](const Config&,const Request& input) {
        if (++calls == turns) last = input;
        return Response{{Role::assistant,body+std::to_string(calls)}};
    }});
    RunOptions options;
    if (window) options.decision_status = DecisionStatusOptions{"long-answer",[] {
        return std::vector{DecisionStatusSource{"host",
            R"([{"key":"host.note","version":1,"description":"原阶段","defaultAttention":{"mode":"explicit"},"valueSchema":{"type":"object","properties":{},"additionalProperties":false}}])",
            R"([{"key":"host.note","version":1,"subject":"stage","fact":1,"value":{}}])"}};
    },false};
    for (unsigned n=1;n<=turns;++n) {
        const auto turn = session.begin("原话"+std::to_string(n),options);
        until(session,[&] { return !session.busy(); });
        check(turn->answer() == body+std::to_string(n),"length pressure interrupted a completed dialogue");
    }
    std::size_t shortened = 0;
    for (const auto& message : last.messages) {
        const auto group = nlohmann::json::parse(message.content,nullptr,false);
        if (!group.is_object() || group.value("source","")!="completed_dialogue") continue;
        for (const auto& turn : group.at("turns")) if (turn.contains("assistant_preview")) {
            const auto& ref = turn.at("assistant_preview");
            const auto original = session.read_evidence(group.at("epoch"),ref.at("message").get<std::size_t>()-1,2);
            check(turn.at("user")==original.front().content && original.back().content.size()==ref.at("bytes") &&
                original.back().content.starts_with(ref.at("prefix").get<std::string>()) && original.back().content.ends_with(ref.at("suffix").get<std::string>()) &&
                original.back().content.find("original-middle")!=std::string::npos && ref.dump().find("original-middle")==std::string::npos,
                "old answer preview lost its source or failed to preserve the full original");
            ++shortened;
        }
    }
    if (window) {
        check(last.messages.back().source==MessageSource::host_status &&
            nlohmann::json::parse(last.messages.back().content).at("definitions").at(0).at("key")=="host.note",
            "compressed request lost the definition of its explicit sample");
        last.messages.pop_back();
    }
    check(shortened && calls==turns && last.messages.front().content=="原话1" && last.messages[1].content==body+"1" &&
        last.messages.back().content=="原话"+std::to_string(turns),"long answer projection changed protected messages");
}

// 按实际协议检查私有续行的预算，切换工具或思考模式后仍可读取原始响应。
// 步骤：保存带长续行的短回答 → 对照无工具和关闭思考的下一次请求 → 核对需要完整续行时的派发边界。
void reasoning_budget() {
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Config config; config.provider = "deepseek"; config.thinking = true;
    const std::string original(1024*1024+1,'r');
    unsigned calls = 0;
    Session session(config,channel,Backend{[&](const Config& actual, const Request& request) {
        if (++calls == 1) return Response{{Role::assistant,"第一条回执",{},{},original}};
        const auto wire = encode_request_fields(request,actual.thinking_enabled());
        check(wire.size()<4096 && wire.find("reasoning_content")==std::string::npos && contains(request,"第一条回执"),
            "unused private continuation entered provider input");
        return Response{{Role::assistant,"已答复"}};
    }});
    auto first = session.begin("首次提问");
    until(session,[&] { return !session.busy(); });
    check(first->operation()->state()==OperationState::succeeded && first->answer()=="第一条回执",
        "unsent private continuation blocked delivery of the visible answer");
    auto without_tools = session.begin("接着问");
    until(session,[&] { return !session.busy(); });
    check(without_tools->answer()=="已答复","unused private continuation blocked tool-free request");

    const std::string id = "test/read";
    ToolCatalog catalog;
    catalog.add({id,1,1,{"read","read",R"({"type":"object","properties":{},"additionalProperties":false})"}},[](const auto&) {
        return completed_tool({true,ToolEffect::not_applied,"fixed",ToolError::none});
    });
    RunOptions options; options.tools = catalog.snapshot(std::span(&id,1)); options.thinking = ThinkingPreference::off;
    const auto thinking_off = session.begin("本轮关闭思考",options);
    until(session,[&] { return !session.busy(); });
    check(thinking_off->answer()=="已答复","unused private continuation blocked request with tools");
    options.thinking = ThinkingPreference::inherit;
    bool blocked = false;
    try { (void)session.begin("本轮需要原续行",options); }
    catch (const std::length_error&) { blocked = true; }
    check(blocked && !session.busy() && calls==3 &&
        session.read_evidence(session.evidence_epoch(),2,1).front().reasoning_content==original,
        "required private continuation bypassed input budget or lost its original");
}
}

// 运行固定响应场景并把实际边界差异交给离线测试。
int main() {
    try {
        source_boundaries(); source_times(); replay(); decision_read(); decision_read_revoked(); counted_responses(); status_versions(); compress_settled(); long_dialogue(false); long_dialogue(true);
        mixed_dialogue(false); mixed_dialogue(true); traceable_recent_dialogue();
        long_answers(26,7500,0); long_answers(12,300,14*1024);
        reasoning_budget();
        batch_capacity(0,0,false); batch_capacity(768,0,true); batch_capacity(768,4096,false);
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
