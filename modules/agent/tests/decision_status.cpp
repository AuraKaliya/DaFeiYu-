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
// 验证非法准备在签发候选前被拒绝。
void rejects(const auto& call, const char* error) {
    try { call(); } catch (const std::logic_error&) { return; }
    throw std::runtime_error(error);
}
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
    auto first = store.prepare(1,2,1,1,100,{source("host","explicit")});
    const auto id = Json::parse(first.full).at("snapshot").get<std::string>();
    publish(store,std::move(first));
    for (auto reply : {store.read(3,2,id),store.read(1,3,id),other.read(1,2,id)})
        check(Json::parse(reply).at("reason") == "scope_mismatch","foreign scope readable");
    publish(other,other.prepare(1,3,1,1,100,{source("host","explicit")}));
    check(Json::parse(other.read(1,3,id)).at("reason") == "scope_mismatch","new epoch exposed original");
    rejects([&] { store.prepare(1,3,1,2,100,{}); },"old store rebound to a new epoch");
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
// 同一事实号的原值和采样期不可覆盖，候选丢弃后释放其未发布的事实预留。
// 步骤：复用原事实并修改其期限 → 检查在途及已发布原件 → 放弃新候选并重新准备。
void original_facts() {
    DecisionStatusStore store;
    auto data = source("host","explicit",7);
    auto first = store.prepare(1,2,1,1,100,{data});
    const auto id = Json::parse(first.full).at("snapshot").get<std::string>();
    auto altered = data;
    auto samples = Json::parse(data.samples);
    samples[0]["validUntilUtcMs"] = 250;
    altered.samples = samples.dump();
    rejects([&] { store.prepare(1,2,1,2,112,{altered}); },"pending fact accepted a new deadline");
    auto shadowed = altered; auto newer = samples[0]; newer["fact"] = 8;
    shadowed.samples = Json::array({newer,samples[0]}).dump();
    rejects([&] { store.prepare(1,2,1,2,112,{shadowed}); },"newer observation hid a rewritten original");
    publish(store,std::move(first));
    auto same = store.prepare(1,2,1,2,112,{data});
    publish(store,std::move(same));
    rejects([&] { store.prepare(1,2,1,3,112,{altered}); },"committed fact accepted a new deadline");
    check(Json::parse(store.read(1,2,id)).at("status").at("items")[0].at("validUntilUtcMs") == 110,"original deadline drifted");
    samples[0]["fact"] = 8; altered.samples = samples.dump();
    { auto preview = store.prepare(1,2,1,3,112,{altered}); }
    samples[0]["validUntilUtcMs"] = 300; altered.samples = samples.dump();
    publish(store,store.prepare(1,2,1,3,112,{altered}));
}
// 核对显式项数量上限、同键多对象顺序和压缩恢复时的声明筛选。
void attention_limit() {
    DecisionStatusStore store;
    auto data = source("host","explicit");
    const auto sample = Json::parse(data.samples)[0];
    auto samples = Json::array();
    for (int i = 8; i >= 0; --i) { auto row = sample; row["subject"] = std::to_string(i); samples.push_back(std::move(row)); }
    data.samples = samples.dump();
    const auto result = store.prepare(1,2,1,1,100,{data,source("mc.companion","implicit")},true);
    const auto tail = Json::parse(result.tail), full = Json::parse(result.full);
    check(tail.at("items").size() == 8 && tail.at("items")[0].at("subject") == "0" &&
        tail.at("items")[7].at("subject") == "7" && tail.at("omittedExplicit") == 1 &&
        tail.at("definitions").size() == 1 && full.at("items").size() == 10 && full.at("definitions").size() == 2,
        "attention count, order or rebuild scope");
}
// 两个任务可独立关注同一个隐式状态；终止或换版后仅原绑定的已发布快照保留原展示。
// 步骤：交叠登记两个任务 → 先后解除并按号复核 → 拒绝越权、错版及缺少身份的登记。
void task_attention() {
    DecisionStatusStore store;
    auto data = source("mc.test","implicit");
    Json a{{"key","mc.test.view"},{"version",1},{"subject","O1"},{"mode","continuous"},{"task","repair"}};
    auto b = a; b["task"] = "deliver";
    data.attention = Json::array({a,b,a}).dump();
    auto first = store.prepare(1,2,3,1,100,{data});
    const auto original = Json::parse(first.tail);
    const auto& items = original.at("items");
    check(items.size()==1 && items[0].at("attention")==Json{{"mode","continuous"},{"tasks",{"deliver","repair"}}} &&
        original.at("definitions").size()==1,"overlapping task watches duplicated or lost the fact");
    publish(store,std::move(first));
    auto other = a; other["subject"] = "O2";
    data.attention = Json::array({b,other}).dump();
    auto next = store.prepare(1,2,3,2,101,{data});
    check(Json::parse(next.tail).at("items")[0].at("attention").at("tasks")==Json::array({"deliver"}),
        "releasing one task cleared another or fabricated an unsampled object");
    publish(store,std::move(next));
    data.attention.clear();
    const auto closed = store.prepare(1,2,4,1,102,{data});
    check(Json::parse(closed.tail).at("items").empty() && Json::parse(closed.full).at("items")[0].at("attention").at("mode")=="implicit" &&
        Json::parse(store.read(1,2,original.at("snapshot").get<std::string>())).at("status").at("items")==items,
        "closing the task removed its source or changed the old snapshot");
    for (const auto& bad : {Json{{"key","host.view"}},Json{{"version",2}},Json{{"task",""}},Json{{"extra",1}}}) {
        auto invalid = a; invalid.update(bad);
        data.attention = Json::array({invalid}).dump();
        rejects([&] { store.prepare(1,2,4,1,103,{data}); },"invalid task watch accepted");
    }
}
// 核对同一事件只唤醒一次，计次只发生于有效应答，历史快照始终保留当时的剩余次数。
// 步骤：预览及放弃 → 重试同一逻辑步并重复回调 → 再展示一次 → 读回结算前的原件。
void counted_attention() {
    DecisionStatusStore store, foreign;
    auto data = source("host","implicit",7);
    Json event{{"mode","counted"},{"key","host.view"},{"version",1},{"subject","O1"},{"event",42},{"fact",7},{"count",2}};
    const auto task = Json{{"mode","continuous"},{"key","host.view"},{"version",1},{"subject","O1"},{"task","repair"}};
    data.attention = Json::array({task,event,event}).dump();
    { auto preview = store.prepare(1,2,3,1,100,{data}); }
    auto first = store.prepare(1,2,3,1,100,{data});
    const auto view = Json::parse(first.tail), id = view.at("snapshot");
    check(view.at("wakeEvents")==Json::array({42}) && view.at("items").size()==1 &&
        view.at("items")[0].at("attention")==Json{{"mode","continuous"},{"tasks",{"repair"}},
            {"events",Json::array({{{"event",42},{"remaining",2}}})}},"overlap lost the original event or duplicated its item");
    auto submitted = store.publish(std::move(first));
    foreign.accept_response(submitted);
    auto retry = store.prepare(1,2,3,1,100,{data},true);
    check(Json::parse(retry.tail).at("items")==view.at("items") && Json::parse(retry.tail).at("wakeEvents")==view.at("wakeEvents"),
        "preview, publication or a foreign response consumed the event");
    auto repeated = store.publish(std::move(retry));
    store.accept_response(submitted); store.accept_response(repeated);
    data.attention = Json::array({event}).dump();
    auto second = store.prepare(1,2,3,2,101,{data});
    const auto tail = Json::parse(second.tail);
    check(tail.at("wakeEvents").empty() && tail.at("items")[0].at("attention")==Json{{"mode","counted"},
        {"events",Json::array({{{"event",42},{"remaining",1}}})}},"retry counted twice or raised a second wake");
    store.accept_response(store.publish(std::move(second)));
    store.accept_response(submitted);
    const auto done = store.prepare(1,2,4,1,102,{data});
    check(Json::parse(done.tail).at("items").empty() && Json::parse(done.full).at("items")[0].at("attention").at("mode")=="implicit" &&
        Json::parse(store.read(1,2,id.get<std::string>())).at("status").at("items")==view.at("items"),"exhausted attention rewrote history");
    event["count"] = 3; data.attention = Json::array({event}).dump();
    rejects([&] { store.prepare(1,2,4,2,103,{data}); },"retransmission replaced original count");
}

// 未入尾注的项和缺失的原样本均不扣次，离开有界窗口的旧事件重传不会重开。
// 步骤：先冻结其他原件 → 裁掉第九项 → 在可展示步骤结算 → 跨过事件及快照窗口再重投。
void counted_limits() {
    DecisionStatusStore store;
    auto data = source("host","implicit",7), filler = source("aaa","explicit");
    Json event{{"mode","counted"},{"key","host.view"},{"version",1},{"subject","O1"},{"event",42},{"fact",7},{"count",1}};
    data.attention = Json::array({event}).dump();
    auto newer = source("host","implicit",8); newer.attention = data.attention;
    auto absent = store.prepare(1,2,1,1,100,{newer});
    check(Json::parse(absent.tail).at("items").empty(),"wake displayed a different fact");
    store.accept_response(store.publish(std::move(absent)));
    auto rows = Json::array(); auto sample = Json::parse(filler.samples)[0];
    for (int i=0; i<8; ++i) { sample["subject"]=std::to_string(i); rows.push_back(sample); }
    filler.samples = rows.dump();
    auto trimmed = store.prepare(1,2,1,2,101,{data,filler});
    check(Json::parse(trimmed.tail).at("omittedExplicit")==1 && Json::parse(trimmed.tail).at("wakeEvents").empty(),
        "an omitted item claimed a wake");
    store.accept_response(store.publish(std::move(trimmed)));
    for (std::uint64_t n=42; n<182; ++n) {
        event["event"]=n; data.attention=Json::array({event}).dump();
        auto current = store.prepare(1,2,2,n,102,{data});
        check(Json::parse(current.tail).at("wakeEvents")==Json::array({n}),"missing or cropped sample consumed the event");
        store.accept_response(store.publish(std::move(current)));
    }
    event["event"]=42; data.attention=Json::array({event}).dump();
    check(Json::parse(store.prepare(1,2,3,1,103,{data}).tail).at("items").empty(),"retired event woke again");
}

// 核对宿主任务引用按 UTF-8 字节计数，空任务不占字段，失败不会改写已发布的作用域。
// 步骤：准备空任务 → 发布最长合法引用 → 拒绝超长和非法编码 → 核对原版读回。
void task_scope() {
    DecisionStatusStore store;
    const auto empty = store.prepare(1,2,1,1,-1,{});
    check(!Json::parse(empty.tail).at("scope").contains("task") && !Json::parse(empty.full).at("scope").contains("task") &&
        Json::parse(empty.full).at("asOf").at("time") == "1969-12-31T23:59:59.999Z","absent task or pre-epoch time");
    const auto task = std::string(125,'x')+"鱼";
    auto scoped = store.prepare(1,2,1,2,0,{},true,task);
    const auto tail = Json::parse(scoped.tail), full = Json::parse(scoped.full);
    const auto id = full.at("snapshot").get<std::string>();
    check(task.size() == 128 && tail.at("scope") == full.at("scope") && full.at("scope").at("task") == task,"task scope mismatch");
    publish(store,std::move(scoped));
    rejects([&] { store.prepare(1,2,1,3,0,{},false,task+"x"); },"129-byte task accepted");
    rejects([&] { store.prepare(1,2,1,3,0,{},false,std::string(1,static_cast<char>(0xff))); },"invalid UTF-8 task accepted");
    check(Json::parse(store.read(1,2,id)).at("status") == full,"read lost task scope");
}
}
// 从公共 C++ 接口离线验收版本、作用域和内存边界。
int main() {
    try { frozen_steps(); retention(); result_limit(); original_facts(); attention_limit(); task_attention(); counted_attention(); counted_limits(); task_scope(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
