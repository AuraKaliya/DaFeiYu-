#include "ask.hpp"
#include <dafeiyu/utf8.hpp>
#include <algorithm>
#include <array>
#include <set>

namespace dfy::app {
namespace {
// 标记尚可等待渠道推进或真人答复的请求。
bool active(AskPhase phase) { return phase == AskPhase::presenting || phase == AskPhase::waiting; }
// 校验正文的 UTF-8 和容量。
bool text_ok(std::string_view text, std::size_t bound) {
    return !text.empty() && text.size() <= bound && runtime::valid_utf8(text) && text.find('\0') == text.npos;
}
}

// 接纳通用提问并固定接收方和渠道。
// 步骤：核对问题与答案契约 → 回收旧回执或拒绝超额 → 发布原票据。
std::uint64_t AskService::open(runtime::DomainId owner, AskSpec spec) {
    if (!owner || !text_ok(spec.question,2048) || !text_ok(spec.recipient,128) || !text_ok(spec.channel,64) ||
        spec.answer_deadline <= clock_() || spec.choices.size() > 8) throw std::invalid_argument("ask: invalid request");
    std::set<std::string_view> distinct;
    for (const auto& choice : spec.choices)
        if (!text_ok(choice,512) || !distinct.insert(choice).second) throw std::invalid_argument("ask: invalid choice");
    if (requests_.size() == 64) {
        const auto old = std::ranges::find_if(requests_,[](const auto& item) { return item.second.released; });
        if (old == requests_.end()) throw std::length_error("ask: pending capacity exhausted");
        requests_.erase(old);
    }
    if (next_ == UINT64_MAX) throw std::overflow_error("ask: identity exhausted");
    requests_.emplace(next_+1,AskRequest{owner,std::move(spec)});
    return ++next_;
}

// 查询原票据的状态和答复。
const AskRequest* AskService::status(std::uint64_t id) const {
    const auto found = requests_.find(id);
    return found == requests_.end() ? nullptr : &found->second;
}

// 验证反馈所使用的原渠道与实例身份。
AskRequest* AskService::bound(std::uint64_t id, runtime::DomainId owner, std::string_view channel) {
    const auto found = requests_.find(id);
    if (found == requests_.end() || found->second.owner != owner || found->second.spec.channel != channel) return nullptr;
    return &found->second;
}
// 先提交实际状态，再通知只读观察者。
void AskService::transition(std::uint64_t id, AskRequest& request, AskPhase phase) noexcept {
    request.phase = phase;
    if (observe_) try { observe_(id,request); } catch (...) {}
}

// 结算原呈现工作，交付后才开放答复入口。
bool AskService::presented(std::uint64_t id, runtime::DomainId owner, std::string_view channel, bool delivered) {
    advance();
    auto* request = bound(id,owner,channel);
    if (!request) return false;
    if (request->phase == AskPhase::presenting) transition(id,*request,delivered ? AskPhase::waiting : AskPhase::failed);
    return delivered ? request->phase == AskPhase::waiting || request->phase == AskPhase::answered : request->phase == AskPhase::failed;
}

// 记录当前渠道允许的真人答案，首份有效结果不可覆盖。
// 步骤：校验期限和接收人 → 识别原有回执 → 校验格式并固定答案。
bool AskService::answer(std::uint64_t id, runtime::DomainId owner, std::string_view channel, std::string_view recipient, std::string text) {
    advance();
    auto* request = bound(id,owner,channel);
    if (!request || request->spec.recipient != recipient) return false;
    if (request->phase == AskPhase::answered) return request->answer == text;
    if (request->phase != AskPhase::waiting || !text_ok(text,2048) ||
        (!request->spec.choices.empty() && std::ranges::find(request->spec.choices,text) == request->spec.choices.end())) return false;
    request->answer = std::move(text);
    transition(id,*request,AskPhase::answered);
    return true;
}

// 撤销仍在等待的原请求并保留取消回执。
void AskService::cancel(std::uint64_t id, runtime::DomainId owner) {
    const auto found = requests_.find(id);
    if (found != requests_.end() && found->second.owner == owner && active(found->second.phase))
        transition(id,found->second,AskPhase::cancelled);
}

// 划定统一时间边界，呈现和答复共享原请求截止时间。
void AskService::advance() {
    const auto now = clock_();
    for (auto& [id,request] : requests_)
        if (active(request.phase) && now >= request.spec.answer_deadline) transition(id,request,AskPhase::expired);
}

// 标出可以回收的终态票据。
void AskService::release(std::uint64_t id) {
    const auto found = requests_.find(id);
    if (found != requests_.end() && !active(found->second.phase)) found->second.released = true;
}

// 公开稳定的票据状态值。
std::string_view ask_phase(AskPhase phase) {
    constexpr std::array labels{"presenting","waiting","answered","expired","cancelled","failed"};
    return labels.at(static_cast<std::size_t>(phase));
}
}
