#include <dafeiyu/observation_input.hpp>
#include <dafeiyu/map_value.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <stdexcept>

namespace dfy::agent {
namespace {
using Json = nlohmann::json;
// 转换一个已核定节点预算的值，嵌套层数另受文本上下文上限约束。
Json encode_value(const runtime::Value& value, std::size_t depth);
// 复制实际存在字段，缺失与显式 null 保持不同，不输出内部引用或代码地址。
// 步骤：核对深度 → 跳过缺失字段并递归编码 → 返回普通数据对象。
Json encode_structure(const runtime::StructValue& value, std::size_t depth) {
    if (depth > 8) throw std::length_error("observation input: nesting limit exceeded");
    Json result = Json::object(); const auto fields = value.descriptor()->fields();
    for (std::size_t index = 0; index < value.size(); ++index) if (value.present(index)) result[fields[index].name] = encode_value(value.at(index),depth+1);
    return result;
}
// 只投影明确支持的数据种类，不把二进制或未知组合猜测成可执行文本。
// 步骤：核对深度 → 直接复制标量 → 递归复制支持的容器 → 拒绝未支持种类。
Json encode_value(const runtime::Value& value, std::size_t depth) {
    using runtime::ValueKind;
    if (depth > 8) throw std::length_error("observation input: nesting limit exceeded");
    switch (value.kind()) {
    case ValueKind::boolean: return value.as_bool();
    case ValueKind::int64: return value.as_int64();
    case ValueKind::uint64: return value.as_uint64();
    case ValueKind::float64: return value.as_float64();
    case ValueKind::string: return value.as_string();
    case ValueKind::null_value: return nullptr;
    case ValueKind::structure: return encode_structure(value.as_structure(),depth);
    case ValueKind::array: {
        Json result = Json::array(); const auto& array = value.as_array();
        for (std::size_t index = 0; index < array.size(); ++index) result.push_back(encode_value(array.at(index),depth+1));
        return result;
    }
    case ValueKind::map: {
        Json result = Json::object(); const auto& map = value.as_map();
        for (std::size_t index = 0; index < map.size(); ++index) { const auto& entry = map.at(index); result[entry.key] = encode_value(entry.value,depth+1); }
        return result;
    }
    default: throw std::invalid_argument("observation input: unsupported value kind");
    }
}
// 以本次请求的时刻标记原来源、对象、修订和取样年龄。
Json observation_metadata(const ObservationMetadata& data, ObservationTime now) {
    using std::chrono::duration_cast;
    using std::chrono::milliseconds;
    return {{"kind","dafeiyu.observation"},{"schema_version",1},{"source",data.source},{"provider_instance",data.provider_instance},
        {"object",{{"id",data.object.id},{"generation",data.object.generation}}},{"revision",data.revision},
        {"trust",data.trust == ObservationTrust::host_verified ? "host_verified" : "external_data"},
        {"age_ms",duration_cast<milliseconds>(now-data.sampled).count()},
        {"valid_for_ms",duration_cast<milliseconds>(std::max(data.expires,now)-now).count()}};
}
// 为已审定用途的元数据追加原结构值，所有读口共用描述和最终正文预算。
std::string encode_observed_payload(const ObservationRecord& observation, Json value) {
    const auto descriptor = observation.payload().descriptor();
    if (descriptor->name().size() > 128 || descriptor->name_bytes() > 64*1024 || descriptor->total_fields() > 1024 || descriptor->depth() > 8)
        throw std::length_error("observation input: descriptor limit exceeded");
    value["payload_schema"] = {{"name",descriptor->name()},{"version",descriptor->version()}};
    value["payload"] = encode_structure(observation.payload(),1);
    auto text = value.dump(); if (text.size() > 64*1024) throw std::length_error("observation input: encoded payload exceeds 64 KiB");
    return text;
}
}
// 编码普通观察数据，来源标签不授予模型任何宿主权限。
// 步骤：核对时效与完整基线 → 编码原来源与有界载荷。
std::string observation_input(const ObservationRecord& observation, ObservationTime now) {
    if (observation.validity(now) != ObservationValidity::fresh || observation.revision_gap())
        throw std::invalid_argument("observation input: fresh complete state required");
    return encode_observed_payload(observation,observation_metadata(observation.metadata(),now));
}
// 为规划复核原取样。在时效内使用普通封套，失效时保留原号与原因供补查。
// 步骤：检查来源及时效 → 编码完整取样或标记缺口 → 返回原身份的数据封套。
std::string plan_observation_input(const ObservationRecord& observation, ObservationTime now) {
    const auto validity = observation.validity(now);
    if (validity == ObservationValidity::fresh && !observation.revision_gap()) return observation_input(observation,now);
    auto value = observation_metadata(observation.metadata(),now);
    switch (validity) {
    case ObservationValidity::expired: value["unavailable"] = "expired"; break;
    case ObservationValidity::source_revoked: value["unavailable"] = "source_revoked"; break;
    case ObservationValidity::future: value["unavailable"] = "future"; break;
    case ObservationValidity::fresh: value["unavailable"] = "revision_gap"; break;
    }
    return value.dump();
}
// 保存当时确实接纳的历史字段，失去来源或尚未发生的样本沿用不可用回执。
// 步骤：复核来源和采样时间 → 标出历史用途、时效和断档 → 编码原接纳的字段。
std::string historical_observation_input(const ObservationRecord& observation, ObservationTime now) {
    const auto validity = observation.validity(now);
    if (validity==ObservationValidity::future || validity==ObservationValidity::source_revoked)
        return plan_observation_input(observation,now);
    auto value = observation_metadata(observation.metadata(),now);
    value["use"] = "historical_event";
    value["validity"] = validity==ObservationValidity::expired ? "expired" : "fresh";
    value["revision_gap"] = observation.revision_gap();
    return encode_observed_payload(observation,std::move(value));
}
}
