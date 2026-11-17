#pragma once
#include <dafeiyu/execution_policy.hpp>
#include <dafeiyu/confirmation.hpp>
#include <dafeiyu/trace_identity.hpp>
#include <dafeiyu/observation_input.hpp>
#include <dafeiyu/plan_definition.hpp>
#include <dafeiyu/action_observations.hpp>

namespace dfy::agent {
// 宿主固定的运行选项，模型输出不得修改目标版本、权限或限额。
struct RunOptions {
    std::optional<ToolSnapshot> tools;
    std::optional<PlanLimits> planning;
    std::size_t goal_update_limit = 4;
    PolicySettings policy;
    std::uint64_t goal_revision = 1;
    RunOrigin origin = RunOrigin::user_request;
    std::chrono::milliseconds timeout{120000};
    std::optional<ToolTime> deadline;
    std::vector<std::shared_ptr<const ObservationRecord>> observations;
    std::optional<ActionObservations> action_observations;
    std::size_t no_progress_limit = 3;
    ToolClock now = std::chrono::steady_clock::now;
    PolicyExtension extension;
    std::optional<ConfirmationCaller> confirmation_caller;
    std::size_t confirmation_limit = 16;
    std::size_t trace_capacity = 256;
    RunVersionLabels version_labels;
    std::shared_ptr<runtime::Budget> budget;
};
}
