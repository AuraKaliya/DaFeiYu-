#include <dafeiyu/observation.hpp>
#include <iostream>

namespace {
using namespace dfy::agent;
using namespace dfy::runtime;
// 报告观察来源、时效或数据寿命错误。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证缺少身份、错误区间或超量载荷明确拒绝。
void rejects(const auto& action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid observation accepted");
}
// 创建宿主不可变结构夹具，正文内容不会成为控制来源。
// 步骤：创建字段描述 → 复制固定正文 → 构造有界不可变载荷。
std::shared_ptr<const StructValue> payload(std::size_t bytes = 32) {
    const FieldDescriptor fields[] = {{"text",ValueKind::string,128*1024}};
    auto descriptor = std::make_shared<const StructDescriptor>(StructDescriptor::create("fixture.observation",1,fields,{1,64}));
    const NamedValue values[] = {{"text",Value::from_string(std::string(bytes,'x'),128*1024)}};
    return std::make_shared<const StructValue>(StructValue::create(descriptor,values,128*1024));
}
// 验证来源身份及信任由宿主固定，未来、到期和撤销不会误判为当前可用。
void boundaries() {
    const ObservationTime now{}; ObservationSource source(8,ObservationTrust::external_data), other(8,ObservationTrust::host_verified);
    const auto value = source.capture({"窗口/一",3},7,now,now+std::chrono::seconds(1),payload());
    check(source.id() != other.id() && value.metadata().source == source.id() && value.metadata().provider_instance == 8 &&
        value.metadata().object.generation == 3 && value.metadata().revision == 7 && value.metadata().trust == ObservationTrust::external_data,
        "observation lost source or object binding");
    check(value.validity(now-std::chrono::nanoseconds(1)) == ObservationValidity::future && value.validity(now) == ObservationValidity::fresh &&
        value.validity(now+std::chrono::seconds(1)) == ObservationValidity::expired,"observation validity boundary incorrect");
    source.revoke(); source.revoke(); check(value.validity(now) == ObservationValidity::source_revoked && value.payload()->payload_bytes() == 32,
        "revocation erased old values or kept them fresh");
    rejects([&] { source.capture({"object",1},1,now,now+std::chrono::seconds(1),payload()); });
}
// 验证旧观察不延续来源授权，非法数据及极值时间不会溢出检查。
void lifetime_and_limits() {
    const ObservationTime now{}; std::optional<ObservationSnapshot> retained;
    {
        ObservationSource source(1,ObservationTrust::host_verified);
        retained = source.capture({"object",1},1,now,now+std::chrono::seconds(1),payload());
        rejects([&] { source.capture({"object",0},1,now,now+std::chrono::seconds(1),payload()); });
        rejects([&] { source.capture({"object",1},0,now,now+std::chrono::seconds(1),payload()); });
        rejects([&] { source.capture({"object",1},1,now,now,payload()); });
        rejects([&] { source.capture({"object",1},1,ObservationTime::min(),ObservationTime::max(),payload()); });
        rejects([&] { source.capture({"object",1},1,now,now+std::chrono::seconds(1),payload(64*1024+1)); });
        rejects([&] { source.capture({"object",1},1,now,now+std::chrono::seconds(1),{}); });
        const auto edge = source.capture({"edge",1},1,ObservationTime::max()-std::chrono::nanoseconds(1),ObservationTime::max(),payload());
        check(edge.validity(ObservationTime::max()) == ObservationValidity::expired,"maximum time overflowed");
    }
    check(retained->validity(now) == ObservationValidity::source_revoked && retained->metadata().revision == 1 && retained->payload(),
        "retained observation prolonged source authority");
    rejects([] { ObservationSource invalid(0,ObservationTrust::external_data); });
}
}
// 仅验证合成观察和真实不可变结构值，不采集环境或调用模型。
int main() {
    try { boundaries(); lifetime_and_limits(); std::cout << "Observation snapshot checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
