#pragma once
#include <dafeiyu/model_messages.hpp>
#include <dafeiyu/operation.hpp>
#include <dafeiyu/run_identity.hpp>
#include <dafeiyu/budget.hpp>
#include <dafeiyu/serial_domain.hpp>
#include <functional>
#include <chrono>
#include <utility>

namespace dfy::agent {
enum class ToolEffect { not_applied, applied, unknown };
enum class ToolError { none, failed, unavailable, invalid_result, cancel_failed, read_failed, release_failed, cancelled, deadline };
struct ToolResult {
    bool succeeded = false;
    ToolEffect effect = ToolEffect::unknown;
    std::string output;
    ToolError error = ToolError::failed;
};
struct ToolProgress { std::optional<ToolResult> result; bool exited = false; ToolError fault = ToolError::none; };
struct ToolInvocation { AttemptId attempt; runtime::DomainId invocation = 0; model::ToolCall call; };
struct ToolCallCounts {
    std::uint64_t proposed = 0, admitted = 0, dispatched = 0;
    // 合计不重叠回合的工具数量，保留三个阶段各自的口径。
    ToolCallCounts& operator+=(const ToolCallCounts& other) noexcept {
        proposed += other.proposed; admitted += other.admitted; dispatched += other.dispatched;
        return *this;
    }
};
using ToolTime = std::chrono::steady_clock::time_point;
using ToolClock = std::function<ToolTime()>;
// 标记首次派发所需的宿主条件及阻塞来源。
enum class ToolStartBlock { none, unavailable, user_input_pending, observation_stale };
// 首次变更检查返回可核对的原因；布尔回调仍可提供原有的允许或拒绝。
struct ToolStartDecision {
    ToolStartBlock block;
    std::uint64_t observation = 0;
    // 将已有布尔检查映射到明确允许或通用前置条件拒绝。
    ToolStartDecision(bool allowed) : block(allowed ? ToolStartBlock::none : ToolStartBlock::unavailable) {}
    // 保存宿主裁定的原因及其关联的原观察绑定。
    ToolStartDecision(ToolStartBlock value, std::uint64_t binding = 0) : block(value), observation(binding) {}
    // 编码拒绝发生的阶段和采样身份，供配对回执和运行追踪共用。
    std::string detail(std::string_view stage) const;
};
using ToolStartGuard = std::function<ToolStartDecision()>;
// 预算覆盖结果保留期，顺序资格一直占用到实际工作退出。
struct ToolResources {
    std::shared_ptr<runtime::SerialDomain> domain;
    std::shared_ptr<const runtime::SerialRequest> ordering;
    runtime::BudgetLease charge;
    std::optional<ToolTime> deadline;
    ToolClock now = std::chrono::steady_clock::now;
    // 纯宿主前置检查，仅在首次调用工厂前执行；拒绝或异常均为确定未发生。
    ToolStartGuard before_start;
};
// 同一宿主线程推进的已接纳工作；后台责任必须保持到 exited，不能在析构中强行中止。
class ToolWork {
  public:
    // 只在实际退出后销毁工作及其代码凭证。
    virtual ~ToolWork() = default;
    // 非阻塞报告结果及实际退出；取消原因可重复传入，首次有效原因由宿主保留。
    virtual ToolProgress poll(runtime::CancelReason cancellation) = 0;
};
// 工厂只在调用期间借用输入；失败不得遗留无人持有的后台工作，实际副作用可能未知。
using ToolFactory = std::function<std::unique_ptr<ToolWork>(const ToolInvocation&)>;
// 将已经完成的同步结果包装为可推进工作，不在包装期间执行任何动作。
std::unique_ptr<ToolWork> completed_tool(ToolResult result);
struct ToolWorkState;
// 工具调用的只读结果；逻辑取消不抹掉随后报告的实际效果。
class ToolRequest {
  public:
    // 禁止复制或移动完成权及调用身份。
    ToolRequest(const ToolRequest&) = delete;
    // 禁止赋值覆盖调用状态。
    ToolRequest& operator=(const ToolRequest&) = delete;
    // 返回宿主唯一业务操作。
    std::shared_ptr<const runtime::Operation> operation() const noexcept { return result_.operation(); }
    // 返回实际工作退出操作，不用业务取消代替。
    std::shared_ptr<const runtime::Operation> exit() const noexcept { return exit_.operation(); }
    // 请求协作取消，清理期间仍可记录首个取消原因。
    runtime::CancelDisposition request_cancel(runtime::CancelReason reason) { return exit_.request_cancel(reason); }
    // 借用调用身份和原始参数，借用期由本请求持有保证。
    const ToolInvocation& invocation() const noexcept { return invocation_; }
    // 只有实际进入注册工具的实现才为真，接纳、排队或启动前拒绝均可独立查证。
    bool dispatched() const noexcept { return dispatched_; }
    // 借用最近报告的效果事实；未开始为未发生，开始后未知直至工作报告。
    const ToolResult& outcome() const noexcept { return outcome_; }
    // 返回宿主观察到的工作协议或收尾故障，独立于已经结算的业务结果。
    ToolError fault() const noexcept { return fault_; }
  private:
    friend struct ToolWorkState;
    // 准备结果和退出操作，不执行工厂或占用原生代码。
    ToolRequest(ToolInvocation invocation, runtime::BudgetLease charge) : charge_(std::move(charge)), invocation_(std::move(invocation)) {}
    runtime::BudgetLease charge_;
    const ToolInvocation invocation_;
    runtime::OperationSource result_, exit_{true};
    ToolResult outcome_{false,ToolEffect::not_applied,{},ToolError::none};
    ToolError fault_ = ToolError::none;
    bool dispatched_ = false;
};
// 已授权且已预留资源的单个工作驱动器；目录授权与调度由上层负责。
class ToolDriver {
  public:
    // 预备调用记录和显式工厂，首次 advance 才调用工厂。
    ToolDriver(ToolInvocation invocation, ToolFactory factory, ToolResources resources = {});
    // 请求停止并尝试推进一次；未退出工作转入有界保留表，不释放在途代码。
    ~ToolDriver();
    // 禁止复制工作推进及退出责任。
    ToolDriver(const ToolDriver&) = delete;
    // 禁止赋值或移动仍在推进的工作。
    ToolDriver& operator=(const ToolDriver&) = delete;
    // 返回请求持有，客户端释放不取消或释放工作。
    std::shared_ptr<ToolRequest> request() const noexcept;
    // 同宿主线程推进一次工厂或工作，异常只结算业务，不能伪造实际退出。
    void advance();
    // 返回需要重试收尾的工作数。
    static std::size_t retained() noexcept;
    // 在原宿主线程重试保留工作，只释放已经确认实际退出的驱动状态。
    static std::size_t retry_retained();
  private:
    friend class ToolSnapshot;
    friend struct ToolWorkState;
    using CheckedFactory = std::function<std::unique_ptr<ToolWork>(const ToolInvocation&,bool&)>;
    // 目录工厂在最终授权和参数准备完成后签认派发，驱动保留同一份只读回执。
    ToolDriver(CheckedFactory factory, ToolInvocation invocation, ToolResources resources);
    std::unique_ptr<ToolWorkState> state_;
};
}
