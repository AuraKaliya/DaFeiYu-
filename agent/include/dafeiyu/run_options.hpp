#pragma once
#include <dafeiyu/execution_policy.hpp>
#include <dafeiyu/confirmation.hpp>
#include <dafeiyu/trace_identity.hpp>
#include <dafeiyu/observation_input.hpp>
#include <dafeiyu/plan_definition.hpp>
#include <dafeiyu/action_observations.hpp>

namespace dfy::agent {
class ObservationController;
// 由观察控制器在条件满足时签发，作用域限于原行动观察绑定和规则修订。
class ObservationActionAuthorization {
  public:
    // 检查原行动依据，零或重采样后的不同绑定都不具备此授权。
    bool matches(runtime::DomainId binding) const noexcept { return binding && binding == binding_; }
    // 返回宿主声明的规则修订，供运行记录实际参与的控制版本。
    std::uint64_t revision() const noexcept { return revision_; }
  private:
    friend class ObservationController;
    // 固定已通过本控制器前置条件的原绑定和版本。
    ObservationActionAuthorization(runtime::DomainId binding, std::uint64_t revision) : binding_(binding),revision_(revision) {}
    runtime::DomainId binding_;
    std::uint64_t revision_;
};
enum class TextHookPoint { input, model_request, model_response, tool_arguments, reply };
using TextHook = std::function<std::optional<std::string>(TextHookPoint,const PolicyContext&,std::string_view,std::string_view,std::size_t)>;
// 宿主固定的运行选项，模型输出不得修改目标版本、权限或限额。
struct RunOptions {
    std::optional<ToolSnapshot> tools;
    std::optional<PlanLimits> planning;
    std::size_t goal_update_limit = 4;
    std::size_t observation_rebind_limit = 16;
    PolicySettings policy;
    std::uint64_t goal_revision = 1;
    RunOrigin origin = RunOrigin::user_request;
    std::chrono::milliseconds timeout{120000};
    std::optional<ToolTime> deadline;
    std::vector<std::shared_ptr<const ObservationRecord>> observations;
    std::optional<ActionObservations> action_observations;
    std::optional<ObservationActionAuthorization> observation_action;
    std::size_t no_progress_limit = 3;
    ToolClock now = std::chrono::steady_clock::now;
    PolicyExtension extension;
    TextHook text_hook;
    std::optional<ConfirmationCaller> confirmation_caller;
    std::size_t confirmation_limit = 16;
    std::size_t trace_capacity = 256;
    RunVersionLabels version_labels;
    std::shared_ptr<runtime::Budget> budget;
    // 宿主为本轮提供的角色引导，仅在构造模型请求时注入。
    std::string system_instructions;
};
}
