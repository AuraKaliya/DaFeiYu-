#include <dafeiyu/decision_status.hpp>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <iostream>
#include <new>
#include <type_traits>

namespace { thread_local bool forbid_allocations = false; }
// 在真实发布调用期间注入分配失败。
void* operator new(std::size_t size) {
    if (forbid_allocations) throw std::bad_alloc();
    if (auto* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
// 释放测试替换分配器的内存。
void operator delete(void* value) noexcept { std::free(value); }
// 释放携带大小提示的测试分配。
void operator delete(void* value, std::size_t) noexcept { std::free(value); }

namespace {
using namespace dfy::agent;
using Json = nlohmann::json;
constexpr std::uint64_t session = UINT64_MAX, epoch = UINT64_MAX-1;
// 报告首个不符合冻结或按版读口契约的结果。
void check(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
// 准备固定来源的状态声明和一个有原采样号、对象及有效期的样本。
DecisionStatusSource source(std::string owner, std::string mode, std::uint64_t fact = 42, std::size_t padding = 0) {
    const auto key = owner + ".view";
    Json schema{{"type","object"},{"properties",{{"note",{{"type","string"}}}}},{"required",{"note"}},{"additionalProperties",false}};
    Json declaration{{"key",key},{"version",1},{"description","观察"},{"defaultAttention",{{"mode",mode}}},{"valueSchema",schema}};
    Json sample{{"key",key},{"version",1},{"subject","O1"},{"fact",fact},{"value",{{"note",std::string(padding,'x')}}},
        {"sampledUtcMs",99},{"validUntilUtcMs",110}};
    return {std::move(owner),Json::array({declaration}).dump(),Json::array({sample}).dump()};
}
// 强制禁止分配后移动候选进索引，同时覆盖首次发布及满槽替换。
void publish(DecisionStatusStore& store, FrozenDecisionStatus frozen) {
    static_assert(!std::is_copy_constructible_v<FrozenDecisionStatus> && noexcept(store.publish(std::move(frozen))));
    forbid_allocations = true;
    store.publish(std::move(frozen));
    forbid_allocations = false;
}
// 核对单步隐显范围、失效、同值重采和原版可读；只读和准备均不推进历史。
// 步骤：冻结后查询未发布版本 → 发布并推进到到期点 → 同值重采 → 核对早期两个版本。
void frozen_steps() {
    DecisionStatusStore store;
    auto host = source("host","explicit"), plugin = source("mc.companion","implicit",43);
    auto first = store.prepare(session,epoch,7,1,100,{plugin,host});
    const auto full = Json::parse(first.full), tail = Json::parse(first.tail);
    const auto id = full.at("snapshot").get<std::string>();
    check(full.at("scope") == Json{{"session",std::to_string(session)},{"epoch",std::to_string(epoch)}} &&
        full.at("schema") == "dafeiyu.agent_status/1" && full.at("turn") == 7 && full.at("step") == 1 &&
        full.at("asOf") == Json{{"event",43},{"time","1970-01-01T00:00:00.100Z"},{"utcMs",100}} && full.at("wakeEvents").empty(),"snapshot header");
    check(tail.at("snapshot") == id && tail.at("items").size() == 1 && tail.at("definitions").size() == 1 &&
        tail.at("items")[0].at("attention").at("mode") == "explicit" && tail.at("items")[0].at("source") == "host" &&
        full.at("items")[1].at("attention").at("mode") == "implicit" && full.at("items")[1].at("source") == "mc.companion","attention/source mismatch");
    check(Json::parse(store.read(session,epoch,id)).at("reason") == "unknown_snapshot","prepare published early");
    first.full.clear(); first.tail.clear();
    publish(store,std::move(first));
    const auto original = store.read(session,epoch,id);
    check(Json::parse(original) == Json{{"state","ready"},{"status",full}} && full.at("items")[0].at("validity") == "current","published original changed");
    host.definitions = "[]";
    auto second = store.prepare(session,epoch,7,2,110,{host});
    const auto expired = Json::parse(second.tail), item = expired.at("items")[0];
    check(expired.at("definitions").empty() && item.at("validity") == "expired" && item.at("fact") == 42 &&
        item.at("sampledUtcMs") == 99 && item.at("validUntilUtcMs") == 110 && expired.at("asOf").at("event") == 43,"wrong expiry boundary");
    publish(store,std::move(second));
    auto rows = Json::parse(host.samples);
    rows[0].update(Json{{"fact",44},{"sampledUtcMs",111},{"validUntilUtcMs",150}});
    host.samples = rows.dump();
    auto third = store.prepare(session,epoch,7,3,112,{host});
    const auto fresh = Json::parse(third.full);
    publish(store,std::move(third));
    check(fresh.at("items")[0].at("value") == item.at("value") && fresh.at("items")[0].at("fact") == 44 &&
        fresh.at("items")[0].at("validity") == "current" && fresh.at("snapshot") != expired.at("snapshot") &&
        store.read(session,epoch,id) == original && Json::parse(store.read(session,epoch,expired.at("snapshot").get<std::string>())).at("status").at("items")[0] == item,
        "resampling rewrote a frozen version");
    auto rebuilt = store.prepare(session,epoch,8,1,112,{host,plugin},true);
    const auto visible = Json::parse(rebuilt.tail), complete = Json::parse(rebuilt.full);
    publish(store,std::move(rebuilt));
    check(visible.at("items").size() == 1 && visible.at("definitions").size() == 1 &&
        visible.at("definitions")[0].at("key") == "host.view" && visible.at("items")[0].at("fact") == 44 &&
        complete.at("items").size() == 2 && complete.at("scope") == full.at("scope") && store.read(session,epoch,id) == original,
        "same-binding rebuild lost scope, disclosure or original");
}
// 证明准备、错域投递和查询不消耗历史槽，只有发布第三十三版才淘汰最早原件。
// 步骤：交叉尝试会话与代次 → 丢弃或错投候选 → 填满索引并验证淘汰边界。
void retention() {
    DecisionStatusStore store, other;
    auto first = store.prepare(1,2,1,1,0,{});
    const auto id = Json::parse(first.full).at("snapshot").get<std::string>();
    publish(store,std::move(first));
    for (auto reply : {store.read(3,2,id),store.read(1,3,id),other.read(1,2,id)})
        check(Json::parse(reply).at("reason") == "scope_mismatch","foreign scope readable");
    for (int i = 0; i < 40; ++i) { auto preview = store.prepare(1,2,1,2,0,{}); }
    auto foreign = store.prepare(1,2,1,2,0,{});
    const auto abandoned = Json::parse(foreign.full).at("snapshot").get<std::string>();
    publish(other,std::move(foreign));
    check(Json::parse(store.read(1,2,abandoned)).at("reason") == "unknown_snapshot","foreign publish accepted");
    for (int i = 1; i < 32; ++i) publish(store,store.prepare(1,2,1,i+1,0,{}));
    check(Json::parse(store.read(1,2,id)).at("state") == "ready","early eviction");
    publish(store,store.prepare(1,2,2,1,0,{}));
    check(Json::parse(store.read(1,2,id)).at("reason") == "outside_retention" &&
        Json::parse(store.read(1,2,abandoned)).at("reason") == "unknown_snapshot","wrong retention reason");
}
// 用真实回包长度构造 64 KiB 和超出一字节的采样，失败后仍可读边界版。
void result_limit() {
    DecisionStatusStore store;
    auto measure = store.prepare(1,2,1,1,100,{source("host","explicit")});
    const auto first_id = Json::parse(measure.full).at("snapshot").get<std::string>();
    publish(store,std::move(measure));
    const auto padding = 64*1024-store.read(1,2,first_id).size();
    auto exact = store.prepare(1,2,1,1,100,{source("host","explicit",43,padding)});
    const auto id = Json::parse(exact.full).at("snapshot").get<std::string>();
    const auto tail = Json::parse(exact.tail);
    check(tail.at("items").empty() && tail.at("omittedExplicit") == 1 && exact.tail.size() <= 16*1024,"oversize explicit item escaped tail");
    publish(store,std::move(exact));
    try { store.prepare(1,2,1,1,100,{source("host","explicit",44,padding+1)}); }
    catch (const std::length_error&) {
        check(store.read(1,2,id).size() == 64*1024,"ToolResult envelope exceeds limit");
        return;
    }
    throw std::runtime_error("oversize ToolResult accepted");
}
}
// 从公共 C++ 接口离线验收版本、作用域和内存边界。
int main() {
    try { frozen_steps(); retention(); result_limit(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
