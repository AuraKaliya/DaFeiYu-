#include <dafeiyu/observation_verification.hpp>
#include <iostream>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
// 报告确定性判定将未知、错误对象或错误类型冒充完成。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 创建单个可选、可空字段，供检查完整标量精度及缺失语义。
std::shared_ptr<const StructValue> payload(Value value, std::uint32_t version = 1, bool missing = false) {
    const auto kind = value.kind() == ValueKind::null_value ? ValueKind::string : value.kind();
    const FieldDescriptor fields[] = {{"state",kind,kind == ValueKind::string ? std::size_t{2048} : std::size_t{0},{},true,true}};
    auto schema = std::make_shared<const StructDescriptor>(StructDescriptor::create("fixture.verification",version,fields,{1,64}));
    const NamedValue values[] = {{"state",value}};
    return std::make_shared<const StructValue>(StructValue::create(schema,missing ? std::span<const NamedValue>{} : std::span<const NamedValue>{values},2048));
}
// 验证支持类型精确比较，不将缺失、空文本或不同整数类型混为相同值。
// 步骤：为各标量建立基线 → 核对相等 → 拒绝旧反馈 → 更换值后核对不满足或类型未知。
void scalars() {
    const std::vector<Value> values{Value::from_bool(true),Value::from_int64(INT64_MIN),Value::from_uint64(UINT64_MAX),Value::from_string("一",8),Value::null()};
    for (const auto& expected : values) {
        ObservationSource source(1,ObservationTrust::host_verified); ObservationInbox inbox({source.id()});
        const auto sample = [&](std::uint64_t revision, Value value) { return source.capture({"object",1},revision,{},ObservationTime{}+std::chrono::seconds(1),payload(value)); };
        inbox.accept(sample(1,expected),{}); const auto original = inbox.guard(inbox.latest(source.id(),"object"),{});
        ObservationExpectation rule(original,"state",expected);
        check(rule.assess(original,{}).match == ObservationMatch::satisfied && rule.assess(original,{},true).issue == ObservationEvidenceIssue::not_resampled,
            "scalar precision or feedback freshness lost");
        inbox.accept(sample(2,expected),{}); const auto current = inbox.guard(inbox.latest(source.id(),"object"),{});
        check(rule.assess(current,{},true).match == ObservationMatch::satisfied && rule.assess(original,{}).match == ObservationMatch::unknown,"new sample did not replace old evidence");
    }
    ObservationSource source(1,ObservationTrust::host_verified); ObservationInbox inbox({source.id()});
    inbox.accept(source.capture({"object",1},1,{},ObservationTime{}+std::chrono::seconds(1),payload(Value::from_uint64(1))),{});
    const auto guard = inbox.guard(inbox.latest(source.id(),"object"),{});
    check(ObservationExpectation(guard,"state",Value::from_uint64(2)).assess(guard,{}).match == ObservationMatch::not_satisfied,"different state treated as unknown or complete");
    check(ObservationExpectation(guard,"state",Value::from_int64(1)).assess(guard,{}).issue == ObservationEvidenceIssue::type_changed,"integer type was coerced");
}
// 验证来源、对象、schema、缺失、时效和采样阶段均不能被模型式相等文本绕过。
// 步骤：固定可信原条件 → 注入不可信或错误证据 → 返回明确未知原因 → 保留原条件。
void evidence() {
    for (unsigned mode = 0; mode < 6; ++mode) {
        ObservationSource source(1,mode == 0 ? ObservationTrust::external_data : ObservationTrust::host_verified);
        ObservationInbox inbox({source.id()}), other({source.id()}); const ObservationTime now{};
        const auto first = source.capture({"object",1},1,now,now+std::chrono::seconds(1),payload(Value::from_uint64(1)));
        inbox.accept(first,now); const auto baseline = inbox.guard(inbox.latest(source.id(),"object"),now); ObservationExpectation rule(baseline,"state",Value::from_uint64(1));
        auto current = baseline;
        if (mode == 1) { other.accept(first,now); current = other.guard(other.latest(source.id(),"object"),now); }
        if (mode == 2 || mode == 3) {
            inbox.accept(source.capture({"object",1},2,now,now+std::chrono::seconds(1),payload(Value::from_uint64(1),mode == 2 ? 2 : 1,mode == 3)),now);
            current = inbox.guard(inbox.latest(source.id(),"object"),now);
        }
        const auto result = rule.assess(current,mode == 4 ? now+std::chrono::seconds(1) : now,false,mode == 5 ? now+std::chrono::milliseconds(1) : now);
        const ObservationEvidenceIssue expected[] = {ObservationEvidenceIssue::untrusted,ObservationEvidenceIssue::wrong_object,ObservationEvidenceIssue::schema_changed,
            ObservationEvidenceIssue::missing_field,ObservationEvidenceIssue::stale,ObservationEvidenceIssue::not_resampled};
        check(result.match == ObservationMatch::unknown && result.issue == expected[mode],"invalid evidence was accepted as target completion");
        bool rejected = false; try { ObservationExpectation unsupported(baseline,"state",Value::from_float64(1.0)); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected,"undefined float tolerance accepted");
    }
}
}
// 只使用宿主结构值验证明确状态条件，不调用模型或执行动作。
int main() {
    try { scalars(); evidence(); std::cout << "Observation verification checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
