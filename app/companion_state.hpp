#pragma once
#include <dafeiyu/tool_executor.hpp>

namespace dfy::app {
enum class CompanionLink { unknown, available, unbound, failed };
struct CompanionSample {
    std::string generation, actor, payload;
    agent::ToolTime received;
};
struct CompanionState {
    std::uint64_t request = 0;
    bool pending = false;
    CompanionLink link = CompanionLink::unknown;
    std::optional<CompanionSample> last;
};

// 宿主可在子模型等待期间直接重读插件登记的世界状态。
class CompanionStateReader {
  public:
    // 对同一在途读取复用编号，新读取只授予一个只读工具。
    std::uint64_t refresh(agent::ToolSnapshot scope, const agent::ToolClock& clock, std::string arguments = "{}");
    // 推进原工作至实际退出，之后才发布新观察或故障。
    void advance();
    // 使当前绑定立即不可用，保留最后一次确认的只读值。
    void revoke();
    // 返回宿主线程拥有的观察记录，不推进工作。
    const CompanionState& state() const noexcept { return state_; }
  private:
    CompanionState state_;
    std::unique_ptr<agent::ToolExecutor> executor_;
    std::shared_ptr<agent::ToolRequest> work_;
    agent::ToolClock clock_;
    bool revoked_ = false;
};
}
