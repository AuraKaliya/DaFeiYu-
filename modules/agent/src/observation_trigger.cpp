#include <dafeiyu/observation_trigger.hpp>
#include <dafeiyu/utf8.hpp>
#include <algorithm>
#include <stdexcept>

namespace dfy::agent {
// 固定有限规则并准备回执空间，构造本身不启用观察触发。
// 步骤：校验来源、对象及预算 → 校验有限时间窗口 → 预留回执并签发规则身份。
ObservationTrigger::ObservationTrigger(ObservationRule rule, ObservationTime created) : rule_(std::move(rule)),created_(created) {
    if (!rule_.source || !rule_.object.generation || rule_.object.id.empty() || rule_.object.id.size() > 128 ||
        !runtime::valid_utf8(rule_.object.id) || !rule_.revision || !rule_.goal_revision || !rule_.maximum_runs || rule_.maximum_runs > 64 ||
        rule_.minimum_interval <= std::chrono::milliseconds::zero() || rule_.minimum_interval > std::chrono::hours(1))
        throw std::invalid_argument("trigger: invalid scope or limits");
    for (const unsigned char byte : rule_.object.id) if (byte < 32 || byte == 127) throw std::invalid_argument("trigger: invalid object");
    const auto hour = std::chrono::duration_cast<ObservationTime::duration>(std::chrono::hours(1));
    const auto end = created > ObservationTime::max()-hour ? ObservationTime::max() : created+hour;
    if (rule_.deadline <= created || rule_.deadline > end) throw std::invalid_argument("trigger: finite deadline required");
    receipts_.reserve(rule_.maximum_runs); id_ = runtime::next_domain();
}
// 销毁规则不销毁原运行，但会使其共享控制门停止放行。
ObservationTrigger::~ObservationTrigger() { stop(); }
// 开启只改变接纳门，不重置已用次数、期限或观察水位。
void ObservationTrigger::enable() {
    if (control_->stopped) throw std::logic_error("trigger: stopped rule cannot restart");
    control_->enabled = true;
}
// 停止为永久决定，已经预留的真实接纳仍可记账。
void ObservationTrigger::stop() noexcept { control_->stopped = true; control_->enabled = false; }
// 返回原规则的生命周期、时间和容量约束，不读取或消费观察值。
TriggerDecision ObservationTrigger::availability(ObservationTime now) const noexcept {
    if (control_->stopped) return TriggerDecision::stopped;
    if (!control_->enabled) return TriggerDecision::disabled;
    if (now < created_) return TriggerDecision::invalid_clock;
    if (now >= rule_.deadline) return TriggerDecision::expired;
    if (pending_) return TriggerDecision::busy;
    if (receipts_.size() == rule_.maximum_runs) return TriggerDecision::run_limit;
    return TriggerDecision::ready;
}
// 核对显式规则和原采样，不从正文推断来源或完整性。
// 步骤：检查规则生命周期及预算 → 核对对象、时效和断档 → 拒绝重复修订或过快触发。
TriggerDecision ObservationTrigger::inspect(const ObservationRecord& value, ObservationTime now) const noexcept {
    const auto state = availability(now); if (state != TriggerDecision::ready) return state;
    const auto& data = value.metadata();
    if (data.source != rule_.source || data.object.id != rule_.object.id || data.object.generation != rule_.object.generation)
        return TriggerDecision::wrong_object;
    if (value.validity(now) != ObservationValidity::fresh) return TriggerDecision::stale;
    if (value.revision_gap()) return TriggerDecision::gap;
    if (!receipts_.empty() && data.revision <= receipts_.back().ticket.observation_revision) return TriggerDecision::duplicate;
    if (next_allowed_ && now < *next_allowed_) return TriggerDecision::throttled;
    return TriggerDecision::ready;
}
// 只准备一个尚未接纳的候选，失败不占用运行次数或推进观察水位。
// 步骤：检查规则及采样 → 签发唯一票据 → 固定尚未执行的预留。
TriggerOffer ObservationTrigger::reserve(const ObservationRecord& value, ObservationTime now) {
    const auto decision = inspect(value,now); if (decision != TriggerDecision::ready) return {decision,{}};
    const TriggerTicket ticket{id_,runtime::next_domain(),value.metadata().revision}; pending_ = Pending{ticket,now}; return {decision,ticket};
}
// 先复用原回执，再记录真实接纳，频率基准不会因外部时钟回退而前移。
// 步骤：校验身份及原回执 → 核对唯一预留 → 固定次数和水位 → 设置下次最早时间。
TriggerReceipt ObservationTrigger::commit(TriggerTicket ticket, RunId run, ObservationTime admitted) {
    if (!run.session || !run.sequence) throw std::invalid_argument("trigger: accepted run required");
    for (const auto& receipt : receipts_) if (receipt.ticket.ticket == ticket.ticket) {
        if (receipt.ticket != ticket || receipt.run != run) throw std::invalid_argument("trigger: receipt conflict");
        return receipt;
    }
    for (const auto& receipt : receipts_) if (receipt.run == run) throw std::invalid_argument("trigger: run already bound to another observation");
    if (!pending_ || pending_->ticket != ticket) throw std::invalid_argument("trigger: unknown reservation");
    const auto base = std::max(admitted,pending_->reserved);
    if (admitted < pending_->reserved) stop();
    const TriggerReceipt receipt{ticket,run}; receipts_.push_back(receipt); pending_.reset();
    const auto delay = std::chrono::duration_cast<ObservationTime::duration>(rule_.minimum_interval);
    next_allowed_ = base > ObservationTime::max()-delay ? ObservationTime::max() : base+delay;
    return receipt;
}
// 撤回只适用于尚未接纳的预留，不撤销或重放已经存在的运行。
void ObservationTrigger::abort(TriggerTicket ticket) {
    if (!pending_ || pending_->ticket != ticket) throw std::invalid_argument("trigger: unknown reservation");
    pending_.reset();
}
// 控制门只保留小型停止状态，不借用规则对象、来源或原生工作。
std::function<bool(ObservationTime)> ObservationTrigger::guard() const {
    return [control = control_,created = created_,deadline = rule_.deadline](ObservationTime now) {
        return control->enabled && !control->stopped && now >= created && now < deadline;
    };
}
}
