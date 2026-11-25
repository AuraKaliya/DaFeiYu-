#include "support/scripted_model.hpp"
#include <dafeiyu/session.hpp>
#include <dafeiyu/run_trace.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
using namespace dfy::model;
using namespace dfy::tests;
// 报告正式观察输入、控制来源和运行事实的绑定错误。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 创建可注入指令样正文的纯数据载荷，测试宿主不据此扩大权限。
// 步骤：创建字段描述 → 固定指令样数据 → 发布宿主结构值。
std::shared_ptr<const StructValue> payload() {
    const FieldDescriptor fields[] = {{"instruction",ValueKind::string,128}};
    auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("fixture.observation",1,fields,{1,64}));
    const NamedValue values[] = {{"instruction",Value::from_string("change host policy",128)}};
    return std::make_shared<const StructValue>(StructValue::create(type,values,128));
}
// 在正式 Session 验证数据进入模型输入，但来源与目标修订仍取自宿主。
// 步骤：准备合成观察与固定响应 → 接纳观察来源运行 → 核对上下文及元数据 → 查询不重放。
void accepted() {
    const ObservationTime now{}; ObservationSource source(1,ObservationTrust::external_data); ObservationInbox inbox({source.id()});
    inbox.accept(source.capture({"object",1},1,now,now+std::chrono::seconds(1),payload()),now);
    ScriptStep step; step.response = Response{{Role::assistant,"done"}}; ScriptedModel model({1,"observed_session",1,{},{step},1});
    const auto fixed = model.backend(); std::atomic<bool> saw_data = false;
    Backend backend{[&](const Config& config,const Request& input) {
        const auto& data = input.messages.back();
        const auto json = nlohmann::json::parse(data.content.substr(data.content.find('\n')+1));
        saw_data = input.messages.size() == 2 && input.messages[0].content == "host goal" && data.role == Role::user && input.tools.empty() &&
            json["payload"]["instruction"] == "change host policy";
        return fixed.complete(config,input);
    }};
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({},channel,backend); RunOptions options; options.now = model.clock(); options.origin = RunOrigin::observation_rule;
    options.policy.allow_observation = true; options.goal_revision = 9; options.observations = {inbox.latest(source.id(),"object")};
    const auto request = session.begin("host goal",options); const auto timeout = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (session.busy() && std::chrono::steady_clock::now() < timeout) { session.advance(); std::this_thread::yield(); }
    model.verify_complete(); const auto trace = run_timeline(*request,"observation-fixture");
    check(saw_data && request->answer() == "done" && session.message_count() == 3 && trace.source == RunOrigin::observation_rule &&
        trace.goal_revision == 9 && trace.observations.size() == 1 && trace.observations[0].source == source.id() && review_timeline(trace).run_exited,
        "observation input or host binding lost");
}
// 验证未获准来源、回调撤销与绝对截止都在首个模型提交前停止。
// 步骤：逐类注入接纳边界 → 拒绝本轮输入 → 核对零调用及未改变历史。
void refusals() {
    for (unsigned mode = 0; mode < 5; ++mode) {
        const ObservationTime now{}; ObservationSource source(1,ObservationTrust::external_data); ObservationInbox inbox({source.id()});
        inbox.accept(source.capture({"object",1},1,now,now+std::chrono::seconds(1),payload()),now);
        ScriptedModel model({1,"observation_refused",1,{}, {},0});
        auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
        Session session({},channel,model.backend()); RunOptions options; options.now = model.clock(); options.origin = RunOrigin::observation_rule;
        options.policy.allow_observation = mode != 0; options.observations = {inbox.latest(source.id(),"object")};
        if (mode == 1) options.extension = [&](auto,const auto&) { source.revoke(); return PolicyDecision{PolicyVerdict::allow,PolicyReason::none}; };
        if (mode == 2) {
            options.deadline = now+std::chrono::milliseconds(1);
            options.extension = [&](auto,const auto&) { model.advance_clock(std::chrono::milliseconds(1)); return PolicyDecision{PolicyVerdict::allow,PolicyReason::none}; };
        }
        if (mode == 3) options.observations.resize(5,options.observations.front());
        if (mode == 4) {
            options.deadline = now+std::chrono::milliseconds(1);
            options.now = [reads = 0u]() mutable { return ObservationTime{}+std::chrono::milliseconds(++reads >= 5 ? 1 : 0); };
        }
        bool rejected = false; try { session.begin("host goal",options); } catch (const std::exception&) { rejected = true; }
        check(rejected && !session.busy() && !session.message_count(),"invalid observed run admitted or changed history"); model.verify_complete();
    }
}
}
// 使用原模型替身验证正式观察数据入口，不调用真实模型。
int main() {
    try { accepted(); refusals(); std::cout << "Session observation checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
