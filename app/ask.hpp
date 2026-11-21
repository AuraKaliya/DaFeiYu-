#pragma once
#include <dafeiyu/tool_work.hpp>
#include <map>
#include <vector>

namespace dfy::app {
enum class AskPhase { presenting, waiting, answered, expired, cancelled, failed };
struct AskSpec {
    std::string question, recipient, channel;
    std::vector<std::string> choices;
    agent::ToolTime answer_deadline;
};
struct AskRequest {
    runtime::DomainId owner;
    AskSpec spec;
    AskPhase phase = AskPhase::presenting;
    std::string answer;
    bool released = false;
};
using AskObserver = std::function<void(std::uint64_t,const AskRequest&)>;

// 管理本体内的异步提问票据，由已注册渠道推进呈现和真人答复。
class AskService {
  public:
    // 固定等待与截止时间的时钟来源。
    explicit AskService(agent::ToolClock clock, AskObserver observe = {}) : clock_(std::move(clock)), observe_(std::move(observe)) {}
    // 创建待呈现请求，返回宿主作用域内的唯一编号。
    std::uint64_t open(runtime::DomainId owner, AskSpec spec);
    // 借用状态直到下一次服务修改。
    const AskRequest* status(std::uint64_t id) const;
    // 确认已实际呈现或记录渠道失败，重复相同回执可安全重试。
    bool presented(std::uint64_t id, runtime::DomainId owner, std::string_view channel, bool delivered);
    // 记录原渠道上同一受信接收人的首份有效答复。
    bool answer(std::uint64_t id, runtime::DomainId owner, std::string_view channel, std::string_view recipient, std::string text);
    // 取消原所有者的未终结请求。
    void cancel(std::uint64_t id, runtime::DomainId owner);
    // 结算到期票据，保留等待中的原身份。
    void advance();
    // 在原插件确认收件或原实例退出后，允许回收该终态票据。
    void release(std::uint64_t id);
  private:
    // 仅借用由该渠道和实例持有的请求。
    AskRequest* bound(std::uint64_t id, runtime::DomainId owner, std::string_view channel);
    // 发布已经生效的票据状态迁移。
    void transition(std::uint64_t id, AskRequest& request, AskPhase phase) noexcept;
    agent::ToolClock clock_;
    AskObserver observe_;
    std::uint64_t next_ = 0;
    std::map<std::uint64_t,AskRequest> requests_;
};
// 将 ASK 状态投影为 Trace 和通信共同使用的固定值。
std::string_view ask_phase(AskPhase phase);
}
