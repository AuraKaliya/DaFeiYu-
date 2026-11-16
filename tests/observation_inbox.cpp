#include <dafeiyu/observation_inbox.hpp>
#include <iostream>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
using A = ObservationAdmission;
// 报告采样合并、水位或计费错误。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 创建确定性的不可变载荷，字段内容不参与来源授权。
// 步骤：声明单字段结构 → 准备固定数值 → 构造不可变载荷。
std::shared_ptr<const StructValue> payload() {
    const FieldDescriptor fields[] = {{"state",ValueKind::uint64,0}};
    auto description = std::make_shared<const StructDescriptor>(StructDescriptor::create("fixture.state",1,fields,{1,64}));
    const NamedValue values[] = {{"state",Value::from_uint64(1)}};
    return std::make_shared<const StructValue>(StructValue::create(description,values,64));
}
// 验证同对象合并、冲突、断档和旧代次拒绝，旧客户端持有仍计入容量。
// 步骤：接纳并保留初值 → 合并和拒绝乱序 → 填满保留预算 → 释放旧值后接受新代次。
void versions_and_budget() {
    ObservationSource source(1,ObservationTrust::external_data); const ObservationTime now{}; const auto data = payload();
    auto parent = std::make_shared<Budget>(BudgetAmount{3,1024}); std::shared_ptr<const ObservationRecord> held;
    {
        ObservationInbox inbox({source.id()},1,{3,1024},parent);
        const auto sample = [&](std::uint64_t revision, std::uint64_t generation = 1) {
            return source.capture({"object",generation},revision,now,now+std::chrono::seconds(1),data);
        };
        const auto first = sample(1); check(inbox.accept(first,now).status == A::inserted,"first sample not admitted");
        held = inbox.latest(source.id(),"object"); check(inbox.accept(first,now).status == A::duplicate && parent->used().items == 1,"duplicate charged twice");
        const auto conflict = source.capture({"object",1},1,now,now+std::chrono::seconds(1),payload());
        check(inbox.accept(conflict,now).status == A::conflict && inbox.latest(source.id(),"object") == held,"same revision rewrote value");
        check(inbox.accept(sample(3),now).revision_gap && inbox.accept(sample(2),now).status == A::stale_revision,"revision gap or stale update lost");
        check(inbox.accept(sample(4),now).revision_gap,"contiguous update silently cleared missing revisions");
        auto second = inbox.latest(source.id(),"object"); check(inbox.accept(sample(5),now).status == A::replaced && parent->used().items == 3,"old records uncharged");
        check(inbox.accept(sample(6),now).status == A::capacity && inbox.latest(source.id(),"object")->metadata().revision == 5,"budget failure changed watermark");
        held.reset(); second.reset(); check(parent->used().items == 1,"old sample charges leaked");
        check(inbox.accept(sample(1,2),now).status == A::replaced && !inbox.latest(source.id(),"object")->revision_gap(),"new generation baseline rejected");
        check(inbox.accept(sample(100),now).status == A::stale_generation,"old generation returned with larger revision");
        const auto other = source.capture({"another",1},1,now,now+std::chrono::seconds(1),data);
        check(inbox.accept(other,now).status == A::capacity,"new object evicted existing watermark");
        held = inbox.latest(source.id(),"object");
    }
    check(parent->used().items == 1 && held->metadata().object.generation == 2,"inbox destruction released client-owned record charge");
    held.reset(); check(!parent->used().items && !parent->used().bytes,"final observation record charge leaked");
}
// 验证来源和时间边界先于数据接纳，过期查询保留事实但不延长新鲜资格。
void source_and_time() {
    ObservationSource source(1,ObservationTrust::host_verified), other(2,ObservationTrust::external_data); const ObservationTime now{};
    ObservationInbox inbox({source.id()},1,{2,1024}); const auto data = payload();
    check(inbox.accept(other.capture({"object",1},1,now,now+std::chrono::seconds(1),data),now).status == A::not_allowed,"ungranted source admitted");
    check(inbox.accept(source.capture({"object",1},1,now+std::chrono::seconds(1),now+std::chrono::seconds(2),data),now).status == A::future,"future sample admitted");
    check(inbox.accept(source.capture({"object",1},1,now-std::chrono::seconds(1),now,data),now).status == A::expired,"expired sample admitted");
    const auto valid = source.capture({"object",1},1,now,now+std::chrono::seconds(1),data); check(inbox.accept(valid,now).status == A::inserted,"valid sample refused");
    check(inbox.latest(source.id(),"object")->validity(now+std::chrono::seconds(1)) == ObservationValidity::expired,"query refreshed stale value");
    ObservationInbox limited({source.id()},1,{2,0}); check(limited.accept(valid,now).status == A::capacity && !limited.latest(source.id(),"object"),"failed byte budget published data");
    source.revoke(); check(inbox.accept(valid,now).status == A::source_revoked,"revoked source admitted");
}
// 验证完整重采样的条件替换，迟到、跨集合、过期及容量失败不能清除断档。
// 步骤：产生断档并固定水位 → 拒绝迟到重采样 → 验证失败保持原值 → 接纳新的完整基线。
void refresh() {
    ObservationSource source(1,ObservationTrust::host_verified); const ObservationTime now{}; const auto data = payload();
    ObservationInbox inbox({source.id()},1,{2,1024}), other({source.id()},1,{2,1024});
    const auto sample = [&](std::uint64_t revision) { return source.capture({"object",1},revision,now,now+std::chrono::seconds(1),data); };
    check(inbox.accept(sample(3),now).revision_gap,"initial missing baseline not visible");
    const auto original = *inbox.refresh_ticket(source.id(),"object"); inbox.accept(sample(4),now);
    check(inbox.refresh(original,sample(5),now).status == A::stale_refresh,"late refresh overwrote new sample");
    const auto expected = *inbox.refresh_ticket(source.id(),"object");
    check(other.refresh(expected,sample(5),now).status == A::stale_refresh,"foreign inbox accepted refresh ticket");
    check(inbox.refresh(expected,sample(5),now+std::chrono::seconds(1)).status == A::expired &&
        inbox.latest(source.id(),"object")->revision_gap(),"expired refresh cleared gap");
    auto held = inbox.latest(source.id(),"object"); inbox.accept(sample(5),now);
    const auto latest = *inbox.refresh_ticket(source.id(),"object");
    check(inbox.refresh(latest,sample(6),now).status == A::capacity && inbox.latest(source.id(),"object")->revision_gap(),"failed refresh lost original gap");
    held.reset(); const auto receipt = inbox.refresh(latest,sample(6),now);
    check(receipt.status == A::replaced && !receipt.revision_gap && !inbox.latest(source.id(),"object")->revision_gap(),"complete refresh failed to establish baseline");
    check(inbox.refresh(latest,sample(6),now).status == A::stale_refresh,"refresh ticket reused after watermarks advanced");
}
}
// 只运行合成状态采样，不把用户命令或动作回执送入可合并集合。
int main() {
    try { versions_and_budget(); source_and_time(); refresh(); std::cout << "Observation inbox checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
