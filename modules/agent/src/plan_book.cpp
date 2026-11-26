#include <dafeiyu/plan_book.hpp>
#include <dafeiyu/utf8.hpp>
#include <algorithm>
#include <stdexcept>

namespace dfy::agent {
// 使用本次运行的原消息时钟，无法取样时不阻断合法的计划状态变更。
std::optional<std::int64_t> PlanBook::timestamp() const noexcept {
    try { return clock_ ? clock_() : message_utc_now(); }
    catch (...) { return {}; }
}
// 当前修订只来自已经完整发布的不可变定义。
PlanBinding PlanBook::binding() const noexcept { return {scope_.run(),scope_.goal_revision(),definition_ ? definition_->binding().plan_revision : 0}; }
// 仅越过已明确完成或跳过的前缀，不把模型完成文字当成推进依据。
std::size_t PlanBook::next() const noexcept {
    std::size_t index = 0;
    while (index < entries_.size() && (entries_[index].state == PlanStepState::completed || entries_[index].state == PlanStepState::skipped)) ++index;
    return index;
}
// 新定义完整准备后才发布，计划修订不能丢弃失败、在途或尚待验证的工作。
// 步骤：核对版本和步骤边界 → 校验新候选及原前缀 → 预留验证空间并保留原事实 → 原子发布。
void PlanBook::install(PlanBinding expected, std::vector<PlanStepDefinition> proposal) {
    if (stopped_ || expected != binding()) throw std::invalid_argument("plan: stopped or stale revision");
    const auto prefix = next();
    if (prefix < entries_.size() && entries_[prefix].state != PlanStepState::pending) throw std::logic_error("plan: work or verification still pending");
    auto definition = std::make_shared<const PlanDefinition>(PlanDefinition::create(scope_,expected.plan_revision+1,std::move(proposal)));
    if (definition->steps().size() < prefix) throw std::invalid_argument("plan: completed prefix removed");
    for (std::size_t index = 0; index < prefix; ++index) if (definition->steps()[index] != definition_->steps()[index])
        throw std::invalid_argument("plan: completed prefix changed");
    std::vector<Entry> prepared(definition->steps().size());
    for (auto& entry : prepared) entry.evidence.reserve(128);
    for (std::size_t index = 0; index < prefix; ++index) prepared[index] = entries_[index];
    const auto when = timestamp();
    for (std::size_t index=prefix; index<prepared.size(); ++index) {
        auto& todo = prepared[index];
        todo.created_utc_ms = todo.updated_utc_ms = when;
        if (!definition_) continue;
        const auto& item = definition->steps()[index];
        const auto& previous = definition_->steps();
        const auto existing = std::ranges::find(previous,item.id,&PlanStepDefinition::id);
        if (existing == previous.end()) continue;
        const auto old = static_cast<std::size_t>(existing-previous.begin());
        todo.created_utc_ms = entries_[old].created_utc_ms;
        if (old == index && *existing == item) todo.updated_utc_ms = entries_[old].updated_utc_ms;
    }
    definition_ = std::move(definition); entries_.swap(prepared);
}
// 精确定位步骤，不改变任何运行或计划状态。
std::size_t PlanBook::find(std::string_view id) const {
    if (definition_) for (std::size_t index = 0; index < definition_->steps().size(); ++index) if (definition_->steps()[index].id == id) return index;
    throw std::invalid_argument("plan: unknown step");
}
// 关联原运行中的下一步骤工作，不接受同一个操作冒充业务结果及实际退出。
// 步骤：核对完整步骤与顺序 → 核对原尝试及独立读句柄 → 固定关联并标记进行中。
void PlanBook::start(const PlanStepKey& key, PlanWork work) {
    if (stopped_ || key.plan != binding()) throw std::invalid_argument("plan: stopped or stale step");
    const auto index = find(key.step);
    if (index != next() || entries_[index].state != PlanStepState::pending) throw std::logic_error("plan: step not ready");
    if (work.execution.step.run != scope_.run() || !work.execution.step.sequence || !work.execution.sequence || !work.result || !work.exit || work.result == work.exit)
        throw std::invalid_argument("plan: original independent work states required");
    for (const auto& previous : entries_) if (previous.work && (previous.work->result == work.result || previous.work->exit == work.exit))
        throw std::invalid_argument("plan: work already assigned to another step");
    auto& entry = entries_[index]; entry.started = key.plan; entry.work = std::move(work); entry.state = PlanStepState::running;
    entry.updated_utc_ms = timestamp();
}
// 证据引用有界且非空白；它的语义由宿主验证器负责，计划不解析为指令。
void PlanBook::validate_evidence(std::string_view value) {
    if (value.empty() || value.size() > 128 || !runtime::valid_utf8(value) || value.find_first_not_of(" \t\r\n") == std::string_view::npos)
        throw std::invalid_argument("plan: bounded host evidence required");
}
// 显式确认只能作用于原成功且已退出工作，原证据重复提交保持同一结果。
// 步骤：核对原步骤版本和证据 → 复用原确认或检查实际状态 → 在预留空间固定宿主验证。
void PlanBook::confirm(const PlanStepKey& key, std::string_view evidence) {
    validate_evidence(evidence); auto& entry = entries_[find(key.step)];
    if (!entry.started || *entry.started != key.plan) throw std::invalid_argument("plan: stale confirmation");
    if (entry.state == PlanStepState::completed) {
        if (entry.evidence != evidence) throw std::invalid_argument("plan: evidence conflict");
        return;
    }
    if (stopped_ || entry.state != PlanStepState::running || entry.work->result->state() != runtime::OperationState::succeeded ||
        entry.work->exit->state() != runtime::OperationState::succeeded) throw std::logic_error("plan: work not verified or not exited");
    entry.evidence.assign(evidence); entry.state = PlanStepState::completed; entry.updated_utc_ms = timestamp();
}
// 明确跳过只作用于下一未开始步骤，不抹去实际失败或取消的工作。
// 步骤：校验证据并复用原决定 → 核对当前版本及顺序 → 固定跳过事实。
void PlanBook::skip(const PlanStepKey& key, std::string_view reason) {
    validate_evidence(reason); const auto index = find(key.step); auto& entry = entries_[index];
    if (entry.state == PlanStepState::skipped && entry.started == key.plan && entry.evidence == reason) return;
    if (stopped_ || key.plan != binding() || index != next() || entry.state != PlanStepState::pending) throw std::logic_error("plan: cannot skip active or stale step");
    entry.started = key.plan; entry.evidence.assign(reason); entry.state = PlanStepState::skipped;
    entry.updated_utc_ms = timestamp();
}
// 首次停止只更新仍受控制的步骤，已完成前缀及重复停止沿用原写入时刻。
void PlanBook::stop() noexcept {
    if (stopped_) return;
    stopped_ = true;
    const auto when = timestamp();
    for (auto& entry : entries_) if (entry.state==PlanStepState::pending || entry.state==PlanStepState::running)
        entry.updated_utc_ms = when;
}
// 原工作仍是结果与退出的唯一来源，计划只记录宿主是否完成验证。
// 步骤：复制不可变定义和控制状态 → 读取每项原操作 → 区分执行、退出、验证及终止等待。
PlanSnapshot PlanBook::snapshot() const {
    PlanSnapshot result{definition_,{},stopped_}; result.steps.reserve(entries_.size());
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        const auto& entry = entries_[index]; PlanStepView view;
        view.key = {entry.started.value_or(binding()),definition_->steps()[index].id}; view.state = entry.state; view.evidence = entry.evidence;
        view.created_utc_ms = entry.created_utc_ms; view.updated_utc_ms = entry.updated_utc_ms;
        if (entry.work) {
            view.execution = entry.work->execution; view.result = entry.work->result->state(); view.exit = entry.work->exit->state();
            if (entry.state == PlanStepState::running) {
                if (*view.result == runtime::OperationState::failed) view.state = PlanStepState::failed;
                if (*view.result == runtime::OperationState::cancelled) view.state = PlanStepState::cancelled;
                view.waiting = *view.exit == runtime::OperationState::pending ? (*view.result == runtime::OperationState::pending ? PlanWait::work : PlanWait::exit) :
                    *view.result == runtime::OperationState::succeeded ? (stopped_ ? PlanWait::stopped : PlanWait::verification) : PlanWait::none;
            }
        } else if (stopped_ && entry.state == PlanStepState::pending) view.waiting = PlanWait::stopped;
        result.steps.push_back(std::move(view));
    }
    return result;
}
}
