#include "decision_status.hpp"
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace dfy::agent::status_detail {
namespace {
// 在解析树增长前限制字节、嵌套和重复字段，数组仅接收有界的状态记录。
// 步骤：核对输入长度 → 逐对象核对字段唯一性 → 验证顶层数组和记录数。
Json rows(const std::string& text) {
    if (text.size() > 64*1024) throw std::length_error("status: source array too large");
    std::vector<std::set<std::string>> keys;
    auto value = Json::parse(text,[&](int depth, Json::parse_event_t event, Json& token) {
        if (depth > 24) throw std::invalid_argument("status: nesting limit");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.back().insert(token.get<std::string>()).second)
            throw std::invalid_argument("status: duplicate field");
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    },false);
    if (!value.is_array()) throw std::invalid_argument("status: JSON array required");
    if (value.size() > 64) throw std::length_error("status: source record limit");
    return value;
}
// 限制记录字段，避免来源自行改写宿主签发的元数据。
void fields(const Json& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) throw std::invalid_argument("status: object required");
    for (const auto& [key,unused] : value.items())
        if (std::find(allowed.begin(),allowed.end(),key) == allowed.end())
            throw std::invalid_argument("status: unknown field");
}
// 读取必填字段，拒绝缺项且不携带输入正文。
const Json& field(const Json& value, const char* name) {
    const auto found = value.find(name);
    if (found == value.end()) throw std::invalid_argument("status: missing field");
    return *found;
}
// 读取有界非空字符串以保留键、对象或说明原文。
const std::string& label(const Json& value, std::size_t limit) {
    if (!value.is_string() || value.get_ref<const std::string&>().empty() ||
        value.get_ref<const std::string&>().size() > limit) throw std::invalid_argument("status: invalid text field");
    return value.get_ref<const std::string&>();
}
// 从 JSON 原始整数读取非零声明版本或事实序号，拒绝符号和浮点近似。
std::uint64_t positive(const Json& value) {
    if (!value.is_number_unsigned() || value == 0) throw std::invalid_argument("status: positive integer required");
    return value.get<std::uint64_t>();
}
// 拒绝超出有符号 UTC 毫秒范围的原始时间。
void check_time(const Json& value) {
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() && value.get<std::uint64_t>() > std::numeric_limits<std::int64_t>::max()))
        throw std::invalid_argument("status: invalid UTC milliseconds");
}
// 核实点分命名空间中的各段非空，并只接受稳定标识字符。
bool dotted_name(std::string_view name) {
    return !name.empty() && name.size() <= 128 && name.front() != '.' && name.back() != '.' &&
        name.find("..") == name.npos && name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == name.npos;
}
// 拒绝超出宿主此次绑定命名空间的声明或采样。
const std::string& owned_key(const Json& row, const std::string& owner) {
    const auto& key = label(field(row,"key"),128);
    if (!dotted_name(key) || !key.starts_with(owner + ".")) throw std::invalid_argument("status: key outside owner namespace");
    return key;
}
// 先按绑定的契约校验值和原时间，再按对象及事实顺序选择当步采样。
// 步骤：核对来源及版本 → 校验对象值和有效期 → 以原采样号去重或更新。
void sample(Frame& frame, const std::string& owner, Json row) {
    fields(row,{"key","version","subject","fact","value","sampledUtcMs","validUntilUtcMs"});
    const auto& key = owned_key(row,owner);
    const auto definition = frame.definitions.find(key);
    if (definition == frame.definitions.end()) throw std::invalid_argument("status: unknown key");
    if (definition->second->declaration.at("owner") != owner ||
        definition->second->version != positive(field(row,"version"))) throw std::invalid_argument("status: wrong owner or version");
    const auto& subject = label(field(row,"subject"),256);
    const auto fact = positive(field(row,"fact"));
    definition->second->schema.validate(field(row,"value").dump());
    for (const auto* time : {"sampledUtcMs","validUntilUtcMs"}) if (row.contains(time)) check_time(row.at(time));
    if (row.contains("sampledUtcMs") && row.contains("validUntilUtcMs") &&
        row.at("validUntilUtcMs").get<std::int64_t>() < row.at("sampledUtcMs").get<std::int64_t>())
        throw std::invalid_argument("status: invalid sample interval");
    row["source"] = owner;
    auto [it,inserted] = frame.samples.try_emplace(std::make_pair(key,subject),row);
    if (!inserted && it->second.at("fact") == fact && it->second.dump() != row.dump())
        throw std::invalid_argument("status: conflicting fact");
    if (!inserted && it->second.at("fact") < fact) it->second = std::move(row);
    frame.cutoff = std::max(frame.cutoff,fact);
}
}
// 校验宿主所有权、默认关注和值契约，同键同版在准备和发布之间保持唯一。
// 步骤：规范化声明 → 核对已占槽及候选 → 编译 schema 并弱预留空槽。
DefinitionRef Registry::reserve(const std::string& owner, Json row) {
    fields(row,{"key","version","description","defaultAttention","valueSchema","owner"});
    const auto& key = owned_key(row,owner);
    if (row.contains("owner") && row.at("owner") != owner) throw std::invalid_argument("status: wrong declaration owner");
    row["owner"] = owner;
    const auto version = positive(field(row,"version"));
    label(field(row,"description"),1024);
    const auto& attention = field(row,"defaultAttention");
    fields(attention,{"mode"});
    const auto& mode = field(attention,"mode");
    if (mode != "explicit" && mode != "implicit") throw std::invalid_argument("status: unknown attention mode");
    const auto canonical = row.dump();
    if (canonical.size() > 16*1024) throw std::length_error("status: declaration too large");
    auto free = slots_.size();
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        const auto other = slots_[i].reserved.lock();
        if (!other) { if (free == slots_.size()) free = i; continue; }
        if (other->declaration.at("key") != key) continue;
        if (other->declaration.at("owner") != owner) throw std::invalid_argument("status: owner conflict");
        if (other->version != version) continue;
        if (other->declaration.dump() != canonical) throw std::invalid_argument("status: conflicting definition");
        return other;
    }
    if (free == slots_.size()) throw std::length_error("status: registry full");
    ToolSchema schema(field(row,"valueSchema").dump());
    const bool visible = mode == "explicit";
    auto entry = std::make_shared<const Definition>(Definition{std::move(row),schema,version,free,visible});
    slots_[free].reserved = entry;
    return entry;
}
// 来源批次共享已提交注册；先收齐本步声明再校验采样，失败的预留随候选释放。
// 步骤：恢复各键的有效版 → 限制来源和总字节 → 登记本步声明 → 收集有界原件。
Frame Registry::collect(const std::vector<DecisionStatusSource>& sources) {
    if (sources.size() > 32) throw std::length_error("status: source limit");
    Frame frame;
    for (const auto& slot : slots_) if (slot.committed) {
        auto& current = frame.definitions[slot.committed->declaration.at("key").get<std::string>()];
        if (!current || current->version < slot.committed->version) current = slot.committed;
    }
    std::size_t bytes = 0, declarations = 0, samples = 0;
    std::set<std::string> supplied;
    for (const auto& source : sources) {
        if (!dotted_name(source.owner)) throw std::invalid_argument("status: invalid owner");
        for (const auto* input : {&source.definitions,&source.samples}) {
            if (input->size() > 64*1024 || input->size() > 256*1024-bytes) throw std::length_error("status: input limit");
            bytes += input->size();
        }
        for (const auto& row : rows(source.definitions)) {
            if (++declarations > 64) throw std::length_error("status: declaration limit");
            auto entry = reserve(source.owner,row);
            const auto& key = entry->declaration.at("key").get_ref<const std::string&>();
            auto& current = frame.definitions[key];
            const bool first = supplied.insert(key).second;
            if (current && (entry->version < current->version || (!first && current->version != entry->version)))
                throw std::invalid_argument("status: conflicting active version");
            current = std::move(entry);
        }
    }
    for (const auto& source : sources) for (const auto& row : rows(source.samples)) {
        if (++samples > 64) throw std::length_error("status: sample limit");
        sample(frame,source.owner,row);
    }
    return frame;
}
// 将已验证版本固定在原槽；其他在准备中的版本仍只由各自候选持有。
void Registry::publish(const Definitions& definitions) noexcept {
    for (const auto& [key,entry] : definitions) slots_[entry->slot].committed = entry;
}
}
