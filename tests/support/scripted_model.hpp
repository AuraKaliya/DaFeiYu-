#pragma once
#include <dafeiyu/async_model.hpp>
#include <dafeiyu/tool_work.hpp>
#include <dafeiyu/utf8.hpp>
#include <algorithm>
#include <mutex>
#include <set>
#include <stdexcept>

namespace dfy::tests {
struct ScriptStep {
    model::Role last_role = model::Role::user;
    std::optional<std::string> last_text, last_tool_id;
    std::vector<std::string> tool_names;
    std::optional<model::Response> response;
    bool fail = false;
    std::chrono::milliseconds advance_by{0};
};
struct ScenarioSpec {
    std::uint32_t schema_version = 1;
    std::string id;
    std::uint32_t version = 1;
    std::vector<std::string> fixtures;
    std::vector<ScriptStep> steps;
    std::optional<std::size_t> expected_model_requests;
};
struct ScriptReport {
    std::string case_id;
    std::uint32_t case_version = 0;
    std::size_t attempted = 0, consumed = 0, violations = 0;
    std::int64_t virtual_elapsed_ms = 0;
};
// 测试专用固定响应入口，不解析执行脚本、不访问服务或读取模型凭据。
class ScriptedModel {
  public:
    // 校验场景、显式调用数和有界序列，声明零调用时也只提供拒绝额外请求的替身。
    // 步骤：核对身份及调用数 → 核对已知夹具 → 校验响应与输入期望 → 保存独立脚本状态。
    ScriptedModel(ScenarioSpec spec, const std::vector<std::string>& known_fixtures = {}) {
        if (spec.schema_version != 1 || !spec.version || spec.id.empty() || spec.id.size() > 64 || spec.steps.size() > 64 ||
            !spec.expected_model_requests || *spec.expected_model_requests != spec.steps.size() || spec.fixtures.size() > 64)
            throw std::invalid_argument("scenario: invalid identity, version or step count");
        for (const auto c : spec.id) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            throw std::invalid_argument("scenario: invalid case ID");
        std::set<std::string> fixtures;
        for (const auto& id : spec.fixtures) if (!fixtures.insert(id).second || std::find(known_fixtures.begin(),known_fixtures.end(),id) == known_fixtures.end())
            throw std::invalid_argument("scenario: unknown or duplicate fixture");
        std::size_t bytes = 0;
        for (const auto& step : spec.steps) {
            if (step.fail == step.response.has_value() || step.advance_by < std::chrono::milliseconds::zero() || step.advance_by > std::chrono::hours(1) ||
                (step.last_role != model::Role::user && step.last_role != model::Role::assistant && step.last_role != model::Role::system && step.last_role != model::Role::tool))
                throw std::invalid_argument("scenario: explicit response or failure required");
            if ((step.last_text && (step.last_text->size() > 1024*1024 || !runtime::valid_utf8(*step.last_text))) ||
                (step.last_tool_id && (step.last_role != model::Role::tool || step.last_tool_id->empty() || step.last_tool_id->size() > 128 || !runtime::valid_utf8(*step.last_tool_id))) ||
                step.tool_names.size() > 64) throw std::invalid_argument("scenario: invalid request expectations");
            std::set<std::string> tools;
            for (const auto& name : step.tool_names) if (name.empty() || name.size() > 64 || !runtime::valid_utf8(name) || !tools.insert(name).second)
                throw std::invalid_argument("scenario: invalid expected tool list");
            if (step.response) {
                model::validate_response(*step.response); bytes += step.response->message.content.size();
                for (const auto& call : step.response->message.tool_calls) bytes += call.id.size()+call.name.size()+call.arguments.size();
            }
            if (bytes > 8*1024*1024) throw std::length_error("scenario: response sequence exceeds 8 MiB");
        }
        state_ = std::make_shared<State>(); state_->spec = std::move(spec);
        state_->report.case_id = state_->spec.id; state_->report.case_version = state_->spec.version;
    }
    // 禁止隐式复制一个场景的消耗责任，后端持有共享状态只用于异步寿命。
    ScriptedModel(const ScriptedModel&) = delete;
    // 禁止赋值或移动覆盖正在运行的场景。
    ScriptedModel& operator=(const ScriptedModel&) = delete;
    // 返回显式替身，配置参数不会用于连接真实服务。
    model::Backend backend() const {
        // 在真实后台核对输入后消耗一个固定响应，失败也保留可核对的计数。
        // 步骤：登记请求 → 核对期望输入 → 推进虚拟时间与游标 → 返回响应或预设异常。
        return {[state = state_](const model::Config&,const model::Request& request) {
            std::lock_guard lock(state->mutex); ++state->report.attempted;
            if (state->report.consumed == state->spec.steps.size()) { ++state->report.violations; throw std::runtime_error("scenario: unexpected model request"); }
            const auto& step = state->spec.steps[state->report.consumed];
            std::vector<std::string> names; for (const auto& tool : request.tools) names.push_back(tool.name);
            auto expected = step.tool_names; std::sort(names.begin(),names.end()); std::sort(expected.begin(),expected.end());
            if (request.messages.empty() || request.messages.back().role != step.last_role ||
                (step.last_text && request.messages.back().content != *step.last_text) ||
                (step.last_tool_id && request.messages.back().tool_call_id != *step.last_tool_id) || names != expected) {
                ++state->report.violations; throw std::runtime_error("scenario: request contract mismatch");
            }
            const auto limit = std::chrono::duration_cast<std::chrono::milliseconds>(agent::ToolTime::max().time_since_epoch()).count();
            if (state->report.virtual_elapsed_ms > limit-step.advance_by.count()) {
                ++state->report.violations; throw std::runtime_error("scenario: virtual clock overflow");
            }
            ++state->report.consumed; state->report.virtual_elapsed_ms += step.advance_by.count();
            if (step.fail) throw std::runtime_error("scenario: scripted model failure");
            return *step.response;
        }};
    }
    // 返回线程安全的虚拟时钟，只由预设响应或测试明确推进。
    agent::ToolClock clock() const {
        return [state = state_] { std::lock_guard lock(state->mutex); return agent::ToolTime{}+std::chrono::milliseconds(state->report.virtual_elapsed_ms); };
    }
    // 由测试明确推进虚拟时间，不休眠或触发任何任务。
    void advance_clock(std::chrono::milliseconds delta) {
        std::lock_guard lock(state_->mutex);
        const auto limit = std::chrono::duration_cast<std::chrono::milliseconds>(agent::ToolTime::max().time_since_epoch()).count();
        if (delta < std::chrono::milliseconds::zero() || delta > std::chrono::hours(1) ||
            state_->report.virtual_elapsed_ms > limit-delta.count()) throw std::invalid_argument("scenario: invalid clock advance");
        state_->report.virtual_elapsed_ms += delta.count();
    }
    // 返回计数和虚拟时间副本，不执行或消耗任何响应。
    ScriptReport report() const { std::lock_guard lock(state_->mutex); return state_->report; }
    // 要求序列完整消耗且无额外或失配调用，避免意外失败冒充预期失败。
    void verify_complete() const {
        std::lock_guard lock(state_->mutex);
        if (state_->report.violations || state_->report.consumed != state_->spec.steps.size()) throw std::runtime_error("scenario: script contract not satisfied");
    }
  private:
    struct State { std::mutex mutex; ScenarioSpec spec; ScriptReport report; };
    std::shared_ptr<State> state_;
};
}
