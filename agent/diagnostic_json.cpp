#include <dafeiyu/diagnostic_json.hpp>
#include "diagnostic_fields.hpp"
#include <nlohmann/json.hpp>
#include <limits>
#include <set>
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
// 读取数组的结构上限，解码时先拒绝超量而不是等构造全部对象后再检查。
template<class T> constexpr std::size_t maximum_items() {
    if constexpr (std::is_same_v<T,TraceNode>) return 512;
    else if constexpr (std::is_same_v<T,TraceFact>) return 1024;
    else if constexpr (std::is_same_v<T,PlanDiagnostic>) return 17;
    else if constexpr (std::is_same_v<T,PlanStepDiagnostic>) return 32;
    else if constexpr (std::is_same_v<T,ObservationInputFact>) return 4;
    else return 64;
}
// 明确类型和范围后读取整数，不接受浮点、布尔或截断转换。
template<class T> T integer(const Json& value) {
    if (!value.is_number_integer()) throw std::invalid_argument("diagnostic: integer required");
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) throw std::invalid_argument("diagnostic: integer overflow");
        return static_cast<T>(number);
    }
    const auto number = value.get<std::int64_t>();
    if constexpr (std::is_unsigned_v<T>) {
        if (number < 0 || static_cast<std::uint64_t>(number) > std::numeric_limits<T>::max()) throw std::invalid_argument("diagnostic: invalid unsigned integer");
    } else if (number < std::numeric_limits<T>::min() || number > std::numeric_limits<T>::max()) throw std::invalid_argument("diagnostic: integer overflow");
    return static_cast<T>(number);
}
// 只解码声明字段的纯值，类型、枚举、数组和额外字段均有明确拒绝。
// 步骤：区分空值及标量 → 核对枚举或时间数值 → 有界读取数组 → 完整读取固定对象字段。
template<class T> T decode(const Json& value) {
    if constexpr (Optional<T>::value) {
        if (value.is_null()) return {}; return decode<typename Optional<T>::item>(value);
    } else if constexpr (std::is_same_v<T,bool>) {
        if (!value.is_boolean()) throw std::invalid_argument("diagnostic: boolean required"); return value.get<bool>();
    } else if constexpr (std::is_integral_v<T>) return integer<T>(value);
    else if constexpr (std::is_same_v<T,std::string>) {
        if (!value.is_string() || value.get_ref<const std::string&>().size() > 128) throw std::invalid_argument("diagnostic: bounded identifier required");
        return value.get<std::string>();
    } else if constexpr (std::is_enum_v<T>) {
        const auto name = decode<std::string>(value);
        for (std::size_t index = 0; index < std::size(EnumNames<T>::values); ++index) if (name == EnumNames<T>::values[index]) return static_cast<T>(index);
        throw std::invalid_argument("diagnostic: unknown enum");
    } else if constexpr (std::is_same_v<T,TurnDiagnostics::TimePoint>) return T(std::chrono::nanoseconds(integer<std::int64_t>(value)));
    else if constexpr (Vector<T>::value) {
        using Item = typename Vector<T>::item;
        if (!value.is_array() || value.size() > maximum_items<Item>()) throw std::invalid_argument("diagnostic: array limit exceeded");
        T result; result.reserve(value.size()); for (const auto& item : value) result.push_back(decode<Item>(item)); return result;
    } else {
        if (!value.is_object()) throw std::invalid_argument("diagnostic: object required");
        T result{}; std::size_t count = 0;
        fields(result,[&](const char* name,auto& field) { ++count; field = decode<std::remove_cvref_t<decltype(field)>>(value.at(name)); });
        if (count != value.size()) throw std::invalid_argument("diagnostic: unknown field"); return result;
    }
}
// 在文件级大小和深度内解析，重复键不能覆盖先前事实。
// 步骤：限制总字节 → 逐对象登记字段 → 拒绝超深和重复键 → 交付纯 JSON 值。
Json parse(std::string_view text) {
    if (text.size() > 8*1024*1024) throw std::length_error("diagnostic: document exceeds 8 MiB");
    std::vector<std::set<std::string>> objects;
    // 按解析事件维护每个对象的键，任何重复或过深输入都直接拒绝。
    // 步骤：检查深度 → 进入／退出对象 → 拒绝同对象重复键。
    return Json::parse(text,[&](int depth,Json::parse_event_t event,Json& value) {
        if (depth > 32) throw std::invalid_argument("diagnostic: nesting limit exceeded");
        if (event == Json::parse_event_t::object_start) objects.emplace_back();
        else if (event == Json::parse_event_t::object_end) objects.pop_back();
        else if (event == Json::parse_event_t::key && !objects.back().insert(value.get<std::string>()).second) throw std::invalid_argument("diagnostic: duplicate field");
        return true;
    });
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
// 导入只还原本文件的相对时钟及值摘要，原生票据和实际运行均没有反序列化入口。
// 步骤：解析有界封套 → 核对格式、时钟和摘要约定 → 读取允许字段 → 复核因果及关联。
DiagnosticSnapshot decode_diagnostic(std::string_view text) {
    try {
        const auto root = parse(text);
        if (!root.is_object() || root.size() != 6 || root.at("kind") != "dafeiyu.diagnostic" || integer<std::uint32_t>(root.at("schema_version")) != 1 ||
            integer<std::uint32_t>(root.at("timeline_schema_version")) != RunTimeline::schema_version ||
            root.at("clock") != "nanoseconds_since_first_model_submission" || root.at("feedback_history") != "latest_assessment_only")
            throw std::invalid_argument("diagnostic: unsupported document format");
        auto result = decode<DiagnosticSnapshot>(root.at("data")); validate_diagnostic_snapshot(result); return result;
    } catch (const Json::exception&) { throw std::invalid_argument("diagnostic: malformed document"); }
}
}
