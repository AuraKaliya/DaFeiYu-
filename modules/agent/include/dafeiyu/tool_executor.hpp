#pragma once
#include <dafeiyu/tool_catalog.hpp>

namespace dfy::agent {
struct ToolLimits { std::size_t calls = 32; std::chrono::milliseconds timeout{30000}; };
// 一个运行拥有的有界工具执行入口；回执保留至运行释放，默认串行且不自动重试。
class ToolExecutor {
  public:
    // 固定运行、权限、预算及期限；可选纯宿主检查在变更工具实际开始前执行。
    ToolExecutor(RunId run, ToolSnapshot tools, ToolLimits limits = {},
        std::shared_ptr<runtime::Budget> parent = {}, ToolClock now = std::chrono::steady_clock::now, std::function<bool()> action_guard = {});
    // 停止接纳并请求收尾；未退出工作由驱动保留原顺序和资源责任。
    ~ToolExecutor();
    // 禁止复制运行的执行与回执责任。
    ToolExecutor(const ToolExecutor&) = delete;
    // 禁止赋值或移动活动执行器。
    ToolExecutor& operator=(const ToolExecutor&) = delete;
    // 校验并接纳调用；同调用 ID、尝试、名称及规范参数复用原回执，冲突拒绝。
    std::shared_ptr<ToolRequest> submit(AttemptId attempt, model::ToolCall call);
    // 推进原顺序域及工具工作，不在一项工作实际退出前放行下一项。
    void advance();
    // 停止新调用并记录取消首因，缓存回执仍可按原命令查询。
    void close(runtime::CancelReason reason = runtime::CancelReason::owner_stop);
    // 返回是否仍有未实际退出的工作，不将已结算业务当作空闲。
    bool busy() const noexcept;
    // 返回已接纳的不同命令数量，成功失败和取消均计入本次运行限额。
    std::size_t size() const noexcept { return size_; }
  private:
    struct Entry {
        std::unique_ptr<ToolDriver> driver;
        std::string canonical;
        runtime::DomainId registration = 0;
    };
    const RunId run_;
    const ToolSnapshot tools_;
    const ToolLimits limits_;
    const ToolClock now_;
    const std::function<bool()> action_guard_;
    std::shared_ptr<runtime::Budget> budget_;
    std::shared_ptr<runtime::SerialDomain> domain_;
    std::vector<Entry> entries_;
    std::size_t size_ = 0;
    bool accepting_ = true, advancing_ = false;
};
}
