#include "companion_state.hpp"
#include <nlohmann/json.hpp>

namespace dfy::app {
namespace {
// 在插件状态协议中只读取共同身份，领域字段完整留给插件或授权展示。
std::string identity(const nlohmann::json& value, const char* key) {
    const auto text = value.at(key).get<std::string>();
    if (text.empty() || text.size() > 128 || text.find_first_of("\r\n") != text.npos || text.find('\0') != text.npos)
        throw std::invalid_argument("companion: invalid observation identity");
    return text;
}
}

// 直接提交宿主状态读取，不启动模型，也不占用子会话的对话回合。
// 步骤：复用在途请求 → 核对单一只读范围 → 创建独立工作并发布编号。
std::uint64_t CompanionStateReader::refresh(agent::ToolSnapshot scope, const agent::ToolClock& clock, std::string arguments) {
    if (revoked_) throw std::logic_error("companion: state source revoked");
    if (state_.pending) return state_.request;
    const auto definitions = scope.definitions();
    if (definitions.size() != 1 || scope.permits_state_changes() || !clock) throw std::invalid_argument("companion: invalid state scope");
    const agent::RunId run{runtime::next_domain(), 1};
    auto executor = std::make_unique<agent::ToolExecutor>(run, std::move(scope), agent::ToolLimits{1, std::chrono::seconds(4)}, nullptr, clock);
    auto work = executor->submit({{run, 1}, 1}, {"state", definitions.front().name, std::move(arguments)});
    clock_ = clock;
    executor_ = std::move(executor);
    work_ = std::move(work);
    state_.request = run.session;
    state_.pending = true;
    return state_.request;
}

// 校验整个原生读取的结果与退出边界，再发布原世界或不可用原因。
// 步骤：推进并等待退出 → 解析身份或撤销值 → 保留上次确认观察并回收工作。
void CompanionStateReader::advance() {
    if (!executor_) return;
    executor_->advance();
    if (work_->exit()->state() == runtime::OperationState::pending) return;
    if (!revoked_) {
        state_.link = CompanionLink::failed;
        const auto& result = work_->outcome();
        if (work_->operation()->state() == runtime::OperationState::succeeded && result.succeeded &&
            result.effect == agent::ToolEffect::not_applied && work_->fault() == agent::ToolError::none && result.output.size() <= 16384) {
            try {
                const auto value = nlohmann::json::parse(result.output);
                if (value.is_null()) state_.link = CompanionLink::unbound;
                else {
                    state_.last = CompanionSample{identity(value, "generation"), identity(value, "actor"), result.output, clock_()};
                    state_.link = CompanionLink::available;
                }
            } catch (const std::exception&) { state_.link = CompanionLink::failed; }
        }
    }
    state_.pending = false;
    work_.reset();
    executor_.reset();
}

// 关闭在途读取的接纳，迟到结果只能归还资源。
void CompanionStateReader::revoke() {
    revoked_ = true;
    state_.link = CompanionLink::unbound;
    if (executor_) executor_->close();
}
}
