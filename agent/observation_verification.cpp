#include <dafeiyu/observation_verification.hpp>
#include <dafeiyu/utf8.hpp>
#include <stdexcept>

namespace dfy::agent {
// 固定有界的明确类型条件，浮点容差和组合语义须另行定义，不能隐式猜测。
// 步骤：检查字段及预期容量 → 限定首版值种类 → 保留原观察基线。
ObservationExpectation::ObservationExpectation(ObservationGuard baseline, std::string field, runtime::Value expected)
    : baseline_(std::move(baseline)),field_(std::move(field)),expected_(expected) {
    using runtime::ValueKind;
    if (field_.empty() || field_.size() > 64 || !runtime::valid_utf8(field_) || expected_.payload_bytes() > 2048)
        throw std::invalid_argument("observation verification: bounded field and expected value required");
    const auto kind = expected_.kind();
    if (kind != ValueKind::boolean && kind != ValueKind::int64 && kind != ValueKind::uint64 && kind != ValueKind::string && kind != ValueKind::null_value)
        throw std::invalid_argument("observation verification: unsupported expected type");
}
// 对完整且可信的当前采样作确定性比较，缺少证据与明确不满足分别返回。
// 步骤：核对来源、水位与采样阶段 → 核对 schema 和字段类型 → 精确比较值 → 返回原采样关联。
ObservationAssessment ObservationExpectation::assess(const ObservationGuard& current, ObservationTime now, bool newer, ObservationTime sampled_after) const {
    using runtime::ValueKind; using Issue = ObservationEvidenceIssue;
    const auto value = current.observation(), baseline = baseline_.observation(); const auto& metadata = value->metadata();
    ObservationAssessment result{ObservationMatch::unknown,Issue::none,metadata.source,metadata.revision};
    if (!current.current(now)) { result.issue = Issue::stale; return result; }
    if (!baseline_.same_object(current)) { result.issue = Issue::wrong_object; return result; }
    if (metadata.trust != ObservationTrust::host_verified) { result.issue = Issue::untrusted; return result; }
    if ((newer && metadata.revision <= baseline->metadata().revision) || metadata.sampled < sampled_after) {
        result.issue = Issue::not_resampled; return result;
    }
    const auto schema = value->payload().descriptor(), original = baseline->payload().descriptor();
    if (schema->name() != original->name() || schema->version() != original->version()) { result.issue = Issue::schema_changed; return result; }
    const auto field = value->payload().find(field_);
    if (!field) { result.issue = Issue::missing_field; return result; }
    if (field->kind() != expected_.kind()) { result.issue = Issue::type_changed; return result; }
    bool matches = false;
    switch (expected_.kind()) {
    case ValueKind::boolean: matches = field->as_bool() == expected_.as_bool(); break;
    case ValueKind::int64: matches = field->as_int64() == expected_.as_int64(); break;
    case ValueKind::uint64: matches = field->as_uint64() == expected_.as_uint64(); break;
    case ValueKind::string: matches = field->as_string() == expected_.as_string(); break;
    case ValueKind::null_value: matches = true; break;
    default: throw std::logic_error("observation verification: invalid stored condition");
    }
    result.match = matches ? ObservationMatch::satisfied : ObservationMatch::not_satisfied; return result;
}
}
