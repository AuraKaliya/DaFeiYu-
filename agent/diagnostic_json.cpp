#include <dafeiyu/diagnostic_json.hpp>
#include "diagnostic_fields.hpp"
#include <nlohmann/json.hpp>
#include <limits>
#include <stdexcept>

namespace dfy::agent {
namespace {
using Json = nlohmann::json;
using namespace diagnostic_detail;
// 递归编码允许列表中的纯值，未定义类型在编译期拒绝。
// 步骤：保留空值与标量 → 编码明确枚举及相对时刻 → 遍历有界数组或声明字段。
template<class T> Json encode(const T& value) {
    if constexpr (Optional<T>::value) return value ? encode(*value) : Json(nullptr);
    else if constexpr (std::is_enum_v<T>) {
        const auto index = static_cast<std::size_t>(value);
        if (index >= std::size(EnumNames<T>::values)) throw std::invalid_argument("diagnostic: unknown enum");
        return std::string(EnumNames<T>::values[index]);
    } else if constexpr (std::is_same_v<T,TurnDiagnostics::TimePoint>) return value.time_since_epoch().count();
    else if constexpr (std::is_integral_v<T> || std::is_same_v<T,std::string>) return value;
    else if constexpr (Vector<T>::value) {
        Json result = Json::array(); for (const auto& item : value) result.push_back(encode(item)); return result;
    } else {
        Json result = Json::object(); fields(value,[&](const char* name,const auto& field) { result[name] = encode(field); }); return result;
    }
}
// 将原宿主单调时刻转为相对纳秒，拒绝溢出而非截断或改变时间顺序。
std::int64_t relative(TurnDiagnostics::TimePoint time, TurnDiagnostics::TimePoint origin) {
    static_assert(std::ratio_equal_v<TurnDiagnostics::TimePoint::duration::period,std::nano>);
    const auto value = time.time_since_epoch().count(), base = origin.time_since_epoch().count();
    if ((base > 0 && value < std::numeric_limits<std::int64_t>::min()+base) ||
        (base < 0 && value > std::numeric_limits<std::int64_t>::max()+base)) throw std::invalid_argument("diagnostic: time offset overflow");
    return value-base;
}
}
// 编码只读诊断并明确时钟及保留范围，不读取配置、凭据或业务正文。
// 步骤：校验已有元数据 → 复制并归一化时刻 → 编码版本化封套 → 检查文件级字节上限。
std::string encode_diagnostic(const DiagnosticSnapshot& snapshot) {
    validate_diagnostic_snapshot(snapshot); auto normalized = snapshot;
    for (auto& fact : normalized.timeline.delta.facts) fact.observed = TurnDiagnostics::TimePoint(std::chrono::nanoseconds(relative(fact.observed,snapshot.timeline.origin)));
    normalized.timeline.origin = {};
    Json root{{"kind","dafeiyu.diagnostic"},{"schema_version",1},{"timeline_schema_version",RunTimeline::schema_version},
        {"clock","nanoseconds_since_first_model_submission"},{"feedback_history","latest_assessment_only"},{"data",encode(normalized)}};
    auto text = root.dump(2); if (text.size() > 8*1024*1024) throw std::length_error("diagnostic: document exceeds 8 MiB"); return text;
}
}
