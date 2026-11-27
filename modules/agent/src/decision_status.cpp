#include "decision_status.hpp"
#include <dafeiyu/runtime_ids.hpp>
#include <dafeiyu/utf8.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <format>
#include <limits>
#include <mutex>
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
        (value.is_number_unsigned() && value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
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
    auto canonical = row.dump();
    const OriginalId id{key,subject,definition->second->version,fact};
    const auto [origin,fresh] = frame.originals.try_emplace(id,std::move(canonical));
    if (!fresh && origin->second != canonical) throw std::invalid_argument("status: conflicting fact");
    auto [it,inserted] = frame.samples.try_emplace(std::make_pair(key,subject),std::move(row));
    if (!inserted && it->second.at("fact") < fact) it->second = std::move(row);
    frame.cutoff = std::max(frame.cutoff,fact);
}
// 将任务或原事件的关注绑定到同来源的声明，计次事件还须指定其原始采样号。
void watch(Frame& frame, const std::string& owner, const Json& row) {
    const auto& key = owned_key(row,owner);
    const auto declaration = frame.definitions.find(key);
    const auto version = positive(field(row,"version"));
    if (declaration == frame.definitions.end() || declaration->second->declaration.at("owner") != owner ||
        declaration->second->version != version)
        throw std::invalid_argument("status: attention declaration mismatch");
    const auto& subject = label(field(row,"subject"),256);
    const auto& mode = field(row,"mode");
    if (mode == "continuous") {
        fields(row,{"mode","key","version","subject","task"});
        frame.tasks[{key,subject}].insert(label(field(row,"task"),128));
    } else if (mode == "counted") {
        fields(row,{"mode","key","version","subject","event","fact","count"});
        const EventKey id{owner,positive(field(row,"event"))};
        CountedWatch grant{key,subject,version,positive(field(row,"fact")),positive(field(row,"count"))};
        const auto [it,fresh] = frame.counted.try_emplace(id,grant);
        if (!fresh && it->second != grant) throw std::invalid_argument("status: conflicting attention event");
    } else throw std::invalid_argument("status: unknown attention grant");
}
}
// 校验宿主所有权、默认关注和值契约，同键同版在准备和发布之间保持唯一。
// 步骤：规范化声明 → 核对已占槽及候选 → 编译 schema 并弱预留空槽。
DefinitionRef Registry::reserve(const std::string& owner, Json row) {
    fields(row,{"key","version","description","defaultAttention","valueSchema","owner","snapshotRead"});
    if (row.contains("snapshotRead") && !row.at("snapshotRead").is_boolean())
        throw std::invalid_argument("status: invalid snapshot read policy");
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
    std::size_t bytes = 0, declarations = 0, samples = 0, watches = 0;
    std::set<std::string> supplied;
    for (const auto& source : sources) {
        if (!dotted_name(source.owner)) throw std::invalid_argument("status: invalid owner");
        for (const auto* input : {&source.definitions,&source.samples,&source.attention}) {
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
    for (const auto& source : sources) {
        if (source.attention.empty()) continue;
        for (const auto& row : rows(source.attention)) {
            if (++watches > 64) throw std::length_error("status: attention limit");
            watch(frame,source.owner,row);
        }
    }
    return frame;
}
// 将已验证版本固定在原槽；其他在准备中的版本仍只由各自候选持有。
void Registry::publish(const Definitions& definitions) noexcept {
    for (const auto& [key,entry] : definitions) slots_[entry->slot].committed = entry;
}

using Version = std::pair<std::string,std::uint64_t>;
using Originals = std::map<OriginalId,std::weak_ptr<const std::string>>;
struct FrozenRecord {
    std::uint64_t binding = 0, serial = 0, cutoff = 0;
    std::pair<std::uint64_t,std::uint64_t> decision;
    std::vector<EventRef> events, displayed;
    std::string snapshot, ready;
    Definitions definitions;
    std::set<Version> visible;
    std::vector<std::shared_ptr<const std::string>> originals;
};
}

namespace dfy::agent {
using namespace status_detail;
struct DecisionStatusStore::Storage {
    std::mutex mutex;
    Registry registry;
    EventAttention attention;
    Originals originals;
    const std::uint64_t binding = runtime::next_domain();
    std::uint64_t session = 0, epoch = 0, issued = 0, cutoff = 0, forgotten = 0;
    std::array<std::shared_ptr<const FrozenRecord>,32> history{};
    std::size_t next = 0;
};
namespace {
// 候选和保留版本共享原件；同一键、对象、契约版及事实号只能对应一份原值与原时间。
// 步骤：回收失去引用的索引 → 对照规范化原采样 → 复用或预留原件并交给候选持有。
void pin_originals(Originals& originals, FrozenRecord& record, Frame& frame) {
    std::erase_if(originals,[](const auto& entry) { return entry.second.expired(); });
    record.originals.reserve(frame.originals.size());
    for (auto& [id,canonical] : frame.originals) {
        auto found = originals.find(id);
        auto prior = found == originals.end() ? nullptr : found->second.lock();
        if (prior && *prior != canonical) throw std::invalid_argument("status: conflicting original fact");
        if (!prior) {
            if (found == originals.end() && originals.size() >= 4096) throw std::length_error("status: original fact limit");
            prior = std::make_shared<const std::string>(std::move(canonical));
            originals.insert_or_assign(id,prior);
        }
        record.originals.push_back(std::move(prior));
    }
}
// 用十进制保留完整身份，独立存储即使收到相同会话参数也有不同的查询前缀。
std::string snapshot_prefix(std::uint64_t session, std::uint64_t epoch, std::uint64_t binding) {
    return std::format("{}/{}/status/{}.",session,epoch,binding);
}
// 以格里高利历的整 400 年周期换算 UTC，保留毫秒和 int64 时间两端的年份。
std::string utc_text(std::int64_t utc_ms) {
    using namespace std::chrono;
    constexpr std::int64_t cycle_ms = 146097LL*86400000;
    const sys_time<milliseconds> point{milliseconds{utc_ms % cycle_ms}};
    const auto year = static_cast<int>(year_month_day{floor<days>(point)}.year()) + 400*(utc_ms/cycle_ms);
    const auto year_text = year >= 0 && year <= 9999 ? std::format("{:04}",year) : std::format("{:+}",year);
    return std::format("{}-{:%m-%dT%T}Z",year_text,point);
}
// 在原值外标记当步可信期，原件中的采样号、时间和业务值保持可核对。
Json at_time(Json sample, std::int64_t utc_ms) {
    if (sample.contains("sampledUtcMs") && utc_ms < sample.at("sampledUtcMs").get<std::int64_t>())
        sample["validity"] = "not_yet_sampled";
    else if (sample.contains("validUntilUtcMs"))
        sample["validity"] = utc_ms < sample.at("validUntilUtcMs").get<std::int64_t>() ? "current" : "expired";
    return sample;
}
// 收集仍需展示且明确绑定本采样的事件；同一对象与任务期关注共用一项。
std::vector<EventRef> matching_events(const FrozenRecord& record, const std::pair<std::string,std::string>& identity,
    std::uint64_t version, std::uint64_t fact) {
    std::vector<EventRef> matches;
    for (const auto& event : record.events) {
        const auto& watch = event->watch;
        if (event->remaining && watch.key==identity.first && watch.subject==identity.second &&
            watch.version==version && watch.fact==fact) matches.push_back(event);
    }
    return matches;
}
// 按稳定键及对象顺序生成可查快照与显式尾注；仅供当前决策的材料不进入快照读口。
// 步骤：冻结原件和有效期 → 附上首次显式展示的契约 → 限制尾注与完整快照大小。
void project(FrozenDecisionStatus& result, FrozenRecord& record, const Frame& frame, const FrozenRecord* previous,
    Json full, std::int64_t utc_ms) {
    Json tail = full;
    std::set<std::string> described;
    std::set<std::uint64_t> wake_ids;
    std::size_t bytes = tail.dump().size() + 32, omitted = 0, decision_only = 0;
    for (const auto& [identity,sample] : frame.samples) {
        const auto& definition = frame.definitions.at(identity.first);
        auto item = at_time(sample,utc_ms);
        const auto tasks = frame.tasks.find(identity);
        item["attention"] = tasks == frame.tasks.end() ? definition->declaration.at("defaultAttention") :
            Json{{"mode","continuous"},{"tasks",tasks->second}};
        const auto events = matching_events(record,identity,definition->version,sample.at("fact").get<std::uint64_t>());
        std::size_t wake_bytes = 0;
        for (const auto& event : events) {
            auto& attention = item.at("attention");
            if (tasks == frame.tasks.end()) attention["mode"] = "counted";
            attention["events"].push_back({{"event",event->event},{"remaining",event->remaining}});
            if (event->remaining == event->watch.count && !wake_ids.contains(event->event))
                wake_bytes += std::to_string(event->event).size()+1;
        }
        if (definition->declaration.value("snapshotRead",true)) {
            full.at("items").push_back(item);
            if (described.insert(identity.first).second) full.at("definitions").push_back(definition->declaration);
        } else ++decision_only;
        if (!definition->explicit_attention && tasks == frame.tasks.end() && events.empty()) continue;
        const Version version{identity.first,definition->version};
        const bool explain = !record.visible.contains(version) && (!previous || !previous->visible.contains(version));
        const auto cost = item.dump().size() + 1 + wake_bytes + (explain ? definition->declaration.dump().size()+1 : 0);
        if (tail.at("items").size() >= 8 || bytes+cost > 16*1024) { ++omitted; continue; }
        bytes += cost;
        record.visible.insert(version);
        if (explain) tail.at("definitions").push_back(definition->declaration);
        tail.at("items").push_back(std::move(item));
        for (const auto& event : events) {
            record.displayed.push_back(event);
            if (event->remaining == event->watch.count && wake_ids.insert(event->event).second)
                tail.at("wakeEvents").push_back(event->event);
        }
    }
    if (omitted) tail["omittedExplicit"] = omitted;
    if (decision_only) full["omittedDecisionOnly"] = decision_only;
    full.at("wakeEvents") = tail.at("wakeEvents");
    result.full = full.dump();
    constexpr std::string_view prefix = R"({"state":"ready","status":)";
    const auto response_bytes = prefix.size() + result.full.size() + 1;
    if (response_bytes > 64*1024) throw std::length_error("status: read response limit");
    record.ready.reserve(response_bytes);
    record.ready.append(prefix).append(result.full).push_back('}');
    result.tail = tail.dump();
}
}
// 为此有效绑定建立固定容量的版本及契约索引。
DecisionStatusStore::DecisionStatusStore() : storage_(std::make_unique<Storage>()) {}
// 释放本绑定的历史和注册；未提交候选不能发布到后续绑定。
DecisionStatusStore::~DecisionStatusStore() = default;
// 在进入提交边界前完成全部校验、编码和索引记录分配，失败不改变可读历史。
// 步骤：核对作用域 → 收集注册和采样 → 冻结两个视图 → 签发只属于本绑定的候选。
FrozenDecisionStatus DecisionStatusStore::prepare(std::uint64_t session, std::uint64_t epoch, std::uint64_t turn,
    std::uint64_t step, std::int64_t utc_ms, const std::vector<DecisionStatusSource>& sources,
    bool rebuild, std::string_view task) const {
    auto& state = *storage_;
    std::lock_guard lock(state.mutex);
    if (state.issued && (state.session != session || state.epoch != epoch)) throw std::invalid_argument("status: scope mismatch");
    if (state.issued == UINT64_MAX) throw std::overflow_error("status: snapshot versions exhausted");
    if (task.size() > 128) throw std::length_error("status: task reference limit");
    if (!runtime::valid_utf8(task)) throw std::invalid_argument("status: invalid task reference");
    auto frame = state.registry.collect(sources);
    auto record = std::make_shared<FrozenRecord>();
    record->binding = state.binding;
    record->decision = {turn,step};
    record->events = state.attention.prepare(frame.counted);
    record->serial = state.issued + 1;
    record->cutoff = std::max(state.cutoff,frame.cutoff);
    record->snapshot = snapshot_prefix(session,epoch,state.binding) + std::to_string(record->serial);
    Json header{{"schema","dafeiyu.agent_status/1"},{"source","host"},{"snapshot",record->snapshot},
        {"scope",{{"session",std::to_string(session)},{"epoch",std::to_string(epoch)}}},{"turn",turn},{"step",step},
        {"asOf",{{"event",record->cutoff},{"time",utc_text(utc_ms)},{"utcMs",utc_ms}}},
        {"wakeEvents",Json::array()},{"items",Json::array()},{"definitions",Json::array()}};
    if (!task.empty()) header.at("scope")["task"] = std::string(task);
    const auto previous = (state.next + state.history.size()-1) % state.history.size();
    FrozenDecisionStatus result;
    project(result,*record,frame,rebuild ? nullptr : state.history[previous].get(),std::move(header),utc_ms);
    pin_originals(state.originals,*record,frame);
    record->definitions = std::move(frame.definitions);
    state.session = session; state.epoch = epoch; state.issued = record->serial;
    result.record_ = std::move(record);
    return result;
}
// 已提交请求只需发布准备好的不可变原件；槽满时按实际发布顺序淘汰。
// 步骤：核对候选归属 → 固定本次注册和事实截止 → 交换历史槽并推进窗口。
PublishedDecisionStatus DecisionStatusStore::publish(FrozenDecisionStatus&& frozen) noexcept {
    auto& state = *storage_;
    if (!frozen.record_ || frozen.record_->binding != state.binding) return {};
    auto record = std::move(frozen.record_);
    std::lock_guard lock(state.mutex);
    state.registry.publish(record->definitions);
    state.attention.publish(record->events);
    state.cutoff = std::max(state.cutoff,record->cutoff);
    auto& slot = state.history[state.next];
    if (slot) state.forgotten = std::max(state.forgotten,slot->serial);
    slot = record;
    state.next = (state.next+1) % state.history.size();
    return record;
}
// 只消费原请求中已展示的计次项，重复或倒序返回的回执不会再次扣除。
void DecisionStatusStore::accept_response(const PublishedDecisionStatus& submitted) noexcept {
    auto& state = *storage_;
    if (!submitted || submitted->binding != state.binding) return;
    std::lock_guard lock(state.mutex);
    for (const auto& event : submitted->displayed) {
        if (!event->remaining || submitted->decision <= event->last_decision) continue;
        --event->remaining;
        event->last_decision = submitted->decision;
    }
}
// 查询仅对匹配的会话、代次和存储开放，直接返回发布时的原版正文。
// 步骤：核对绑定前缀 → 寻找精确版本 → 区分保留窗口外和未知版本。
std::string DecisionStatusStore::read(std::uint64_t session, std::uint64_t epoch, std::string_view snapshot) const {
    auto& state = *storage_;
    std::lock_guard lock(state.mutex);
    if (snapshot.size() > 128) return R"({"state":"unavailable","reason":"invalid_snapshot"})";
    const auto prefix = snapshot_prefix(session,epoch,state.binding);
    if ((state.issued && (session != state.session || epoch != state.epoch)) ||
        (snapshot.find("/status/") != snapshot.npos && !snapshot.starts_with(prefix)))
        return R"({"state":"unavailable","reason":"scope_mismatch"})";
    for (const auto& record : state.history) if (record && record->snapshot == snapshot) return record->ready;
    if (snapshot.starts_with(prefix)) {
        const auto suffix = snapshot.substr(prefix.size());
        std::uint64_t serial = 0;
        const auto [end,ec] = std::from_chars(suffix.data(),suffix.data()+suffix.size(),serial);
        if (ec == std::errc{} && end == suffix.data()+suffix.size() && suffix.front() != '0' && serial <= state.forgotten)
            return R"({"state":"unavailable","reason":"outside_retention"})";
    }
    return R"({"state":"unavailable","reason":"unknown_snapshot"})";
}
}
