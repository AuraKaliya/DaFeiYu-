#include "../src/decision_status.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace dfy::agent;
using namespace dfy::agent::status_detail;
// 报告第一个偏离注册或采样契约的断言。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证错误输入在产生可发布帧之前被拒绝。
void rejects(const auto& action) {
    try { action(); } catch (const std::invalid_argument&) { return; } catch (const std::length_error&) { return; }
    throw std::runtime_error("invalid status accepted");
}
// 准备一个使用绑定命名空间、封闭值结构和两个独立事实号的来源。
DecisionStatusSource source() {
    return {"mc.companion",R"([{"key":"mc.companion.block","version":1,"description":"方块观察","defaultAttention":{"mode":"explicit"},"valueSchema":{"type":"object","properties":{"seen":{"type":"integer"}},"required":["seen"],"additionalProperties":false}}])",
        R"([{"key":"mc.companion.block","version":1,"subject":"O1","fact":7,"value":{"seen":0}},{"key":"mc.companion.block","version":1,"subject":"O1","fact":5,"value":{"seen":2}}])"};
}
// 验证注册与采样独立、按事件顺序取原件以及同版声明的不可变性。
// 步骤：准备后放弃 → 发布登记并单独采样 → 拒绝同版改写 → 升版并拒绝旧采样。
void registrations() {
    Registry registry;
    auto input = source();
    auto definitions = Json::parse(input.definitions);
    auto sample_only = input;
    sample_only.definitions = "[]";
    { const auto preview = registry.collect({input}); }
    rejects([&] { registry.collect({sample_only}); });
    const auto first = registry.collect({input});
    registry.publish(first.definitions);
    const auto current = registry.collect({sample_only});
    const auto& item = current.samples.at({"mc.companion.block","O1"});
    check(current.samples.size() == 1 && current.cutoff == 7 && item.at("fact") == 7 &&
        item.at("source") == input.owner && item.at("value").at("seen") == 0,"lost original fact");
    definitions[0]["description"] = "同版替换";
    input.definitions = definitions.dump();
    rejects([&] { registry.collect({input}); });
    definitions[0]["version"] = 2;
    input.definitions = definitions.dump();
    rejects([&] { registry.collect({input}); });
    input.samples = "[]";
    registry.publish(registry.collect({input}).definitions);
    rejects([&] { registry.collect({sample_only}); });
}
// 验证来源隔离、原始 JSON 结构、值 schema 与时间边界。
void invalid_inputs() {
    Registry registry;
    const auto good = source();
    auto bad = good;
    bad.owner = "mc.other";
    rejects([&] { registry.collect({bad}); });
    bad = good; bad.definitions = "{}"; rejects([&] { registry.collect({bad}); });
    bad.definitions = "[{\"key\":\"a\",\"key\":\"b\"}]"; rejects([&] { registry.collect({bad}); });
    bad.definitions.assign(65537,' '); rejects([&] { registry.collect({bad}); });
    auto definitions = Json::parse(good.definitions);
    definitions[0]["owner"] = "host";
    bad.definitions = definitions.dump(); rejects([&] { registry.collect({bad}); });
    const auto original = Json::parse(good.samples)[0];
    for (const auto& patch : {Json{{"value",{{"seen","0"}}}},Json{{"value",Json::object()}},Json{{"fact",0}},
             Json{{"fact",-1}},Json{{"fact",1.0}},Json{{"subject",""}},Json{{"source","host"}},
             Json{{"sampledUtcMs",12},{"validUntilUtcMs",11}},Json{{"validUntilUtcMs",UINT64_MAX}}}) {
        auto row = original; row.update(patch);
        bad = good; bad.samples = Json::array({row}).dump(); rejects([&] { registry.collect({bad}); });
    }
    auto other = original; other["value"]["seen"] = 1;
    bad.samples = Json::array({original,other}).dump(); rejects([&] { registry.collect({bad}); });
    auto newer = original; newer["fact"] = 8;
    bad.samples = Json::array({newer,original,other}).dump(); rejects([&] { registry.collect({bad}); });
}
// 用最后一个注册槽验证失败整批回滚以及丢弃候选不会耗尽绑定。
// 步骤：固定 63 个声明 → 在末槽拒绝两项批次 → 放弃单项候选 → 发布第 64 项。
void registry_limit() {
    Registry registry;
    auto input = source();
    input.samples = "[]";
    const auto prototype = Json::parse(input.definitions)[0];
    auto many = Json::array();
    for (int i = 0; i < 63; ++i) {
        auto definition = prototype;
        definition["key"] = input.owner + ".field" + std::to_string(i);
        many.push_back(std::move(definition));
    }
    input.definitions = many.dump();
    registry.publish(registry.collect({input}).definitions);
    auto a = prototype, b = prototype;
    a["key"] = input.owner + ".a"; b["key"] = input.owner + ".b";
    input.definitions = Json::array({a,b}).dump(); rejects([&] { registry.collect({input}); });
    input.definitions = Json::array({a}).dump();
    { const auto preview = registry.collect({input}); }
    input.definitions = Json::array({b}).dump();
    registry.publish(registry.collect({input}).definitions);
    input.definitions = Json::array({a}).dump(); rejects([&] { registry.collect({input}); });
    rejects([&] { registry.collect(std::vector<DecisionStatusSource>(33,input)); });
    check(registry.collect({}).definitions.size() == 64,"registry reservation survived failed preparation");
}
}
// 用固定本地 JSON 验证通用状态契约。
int main() {
    try { registrations(); invalid_inputs(); registry_limit(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
