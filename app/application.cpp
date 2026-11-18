#include "application.hpp"
#include <dafeiyu/model_client.hpp>
#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>

namespace dfy::app {
namespace {
// 从明确的包工具 ID 取得包名称，避免根据局部名称选择实例。
std::string_view package_of(std::string_view id) {
    const auto separator = id.find('/');
    if (separator == std::string_view::npos || !separator || separator+1 == id.size())
        throw std::invalid_argument("tool: expected package/tool ID");
    return id.substr(0,separator);
}

// 校验无参数契约，确保前后两次读针对同一个插件状态。
void require_parameterless(const agent::ToolSnapshot& scope) {
    const auto tools = scope.definitions();
    if (tools.size() != 1 || !nlohmann::json::parse(tools.front().parameters).at("properties").empty())
        throw std::invalid_argument("verification: parameterless tools required");
}
}

// 装配内置结构化 Provider，普通问答沿用同一后端和上下文。
Application::Application(model::Config config, const std::filesystem::path& packages, const std::filesystem::path& user_data)
    : Application(std::move(config),packages,user_data,model::Backend{model::complete_response}) {}

// 让正式会话与插件共享同一有界事件通道。
// 步骤：准备通道 → 创建插件目录与会话 → 绑定会话服务。
Application::Application(model::Config config, const std::filesystem::path& packages, const std::filesystem::path& user_data,
    model::Backend backend)
    : channel_(std::make_shared<events::BroadcastChannel>(std::make_shared<events::EventStore>(256,64),16,8)),
      plugins_(packages,user_data,channel_), session_(std::move(config),channel_,std::move(backend)),
      conversation_(session_) {}

// 为用户指明的一个只读工具接纳运行。
agent::TurnDiagnostics Application::submit_read_only(const std::string& tool_id, const std::string& prompt) {
    return submit_tool(tool_id,prompt,false);
}

// 由用户指明一个工具，写入必须经过具体调用的确认。
agent::TurnDiagnostics Application::submit_confirmed(const std::string& tool_id, const std::string& prompt) {
    return submit_tool(tool_id,prompt,true);
}

// 从活动实例准备本轮的工具注册。
// 步骤：核对会话空闲 → 定位原活动实例 → 在临时目录注册并返回所有权。
Application::PreparedTools Application::prepare_tools(std::string_view package) {
    if (conversation_.busy()) throw std::logic_error("session: wait for the current request to finish");
    const auto instance = plugins_.instance(std::string(package));
    if (instance->phase() != plugins::InstancePhase::active) throw std::invalid_argument("tool: plugin instance is not active");
    auto catalog = std::make_shared<agent::ToolCatalog>(64);
    auto registration = std::make_unique<plugins::PluginTools>(instance,catalog);
    return {instance,std::move(catalog),std::move(registration)};
}

// 从用户指定的已加载实例授予一个工具。
// 步骤：准备原实例注册 → 固定确认策略和范围 → 接纳并保留原注册。
agent::TurnDiagnostics Application::submit_tool(const std::string& tool_id, const std::string& prompt, bool confirm_changes) {
    auto prepared = prepare_tools(package_of(tool_id));
    agent::RunOptions options;
    options.tools = prepared.catalog->snapshot(std::span(&tool_id,1),confirm_changes);
    options.policy.confirm_changes = confirm_changes;
    if (confirm_changes) options.confirmation_caller = caller_;

    const auto accepted = conversation_.submit(prompt,std::move(options));
    tool_registration_.emplace(ToolRegistration{accepted.turn,prepared.instance,std::move(prepared.registration)});
    return accepted;
}

// 在原 Session 中开启有实际读回条件的单步任务。
// 步骤：核对同实例及无参读写契约 → 固定原运行预算和证明条件 → 接纳后绑定工具与验证责任。
agent::TurnDiagnostics Application::submit_verified(const std::string& reader, const std::string& change, const std::string& expected,
    const std::string& prompt) {
    if (prompt.find_first_not_of(" \t\r\n") == std::string::npos) throw std::invalid_argument("verification: user goal required");
    const auto package = package_of(reader);
    if (package_of(change) != package) throw std::invalid_argument("verification: tools must use the same plugin instance");
    auto prepared = prepare_tools(package);
    require_parameterless(prepared.catalog->snapshot(std::span(&reader,1)));
    const auto write_scope = prepared.catalog->snapshot(std::span(&change,1),true);
    require_parameterless(write_scope);
    if (!write_scope.permits_state_changes()) throw std::invalid_argument("verification: state-changing tool required");

    agent::RunOptions options;
    const std::array ids{reader,change};
    options.tools = prepared.catalog->snapshot(ids,true);
    options.planning = agent::PlanLimits{1,1};
    options.confirmation_caller = caller_;
    options.policy.confirm_changes = true;
    options.policy.tool_limit = 3;
    options.confirmation_limit = 1;
    const auto deadline = options.now()+options.timeout;
    options.deadline = deadline;
    auto verifier = std::make_unique<ToolVerification>(reader,change,expected,prepared.instance->generation(),deadline);

    const auto goal = prompt+"\nWithin one plan step, make exactly three separate tool calls in this order: "+
        reader+", "+change+", "+reader+". Then summarize the outcome. The host verifies the final state.";
    const auto accepted = conversation_.submit(goal,std::move(options));
    tool_registration_.emplace(ToolRegistration{accepted.turn,prepared.instance,std::move(prepared.registration)});
    verification_.emplace(VerificationReport{{accepted.session,accepted.turn},{}});
    verifier_ = std::move(verifier);
    return accepted;
}

// 只报告与当前或最近一次运行关联的证据状态。
std::optional<VerificationReport> Application::verification() const noexcept {
    const auto status = conversation_.status();
    if (!status || !verification_ || verification_->run != agent::RunId{status->session,status->turn}) return {};
    auto report = *verification_;
    if (!report.match && status->exit != runtime::OperationState::pending) report.match = agent::ObservationMatch::unknown;
    return report;
}

// 从原活动回合读取确认值。
std::vector<agent::ConfirmationSnapshot> Application::confirmations() const {
    if (conversation_.current_) return conversation_.current_->confirmations();
    return {};
}

// 由宿主固定审批对象，用户命令只提供请求、修订和决定。
// 步骤：撤销失效实例 → 查找原请求 → 以请求身份标识决定并交给会话复核。
agent::ConfirmationReceipt Application::decide(runtime::DomainId request, std::uint64_t revision, bool approve) {
    collect_tool_registration();
    advance_verification();
    const auto values = confirmations();
    const auto found = std::find_if(values.begin(),values.end(),[request](const auto& value) { return value.id == request; });
    if (found == values.end()) throw std::invalid_argument("confirmation: request unavailable");
    if (approve && found->state == agent::ConfirmationState::waiting && verification_ &&
        found->target.attempt.step.run == verification_->run &&
        (!verifier_ || !verifier_->can_approve(found->target,std::chrono::steady_clock::now())))
        throw std::invalid_argument("verification: original read evidence is unavailable");

    return session_.decide(caller_,{request,request,revision,found->target,approve});
}

// 按原运行身份与实例寿命撤销已失效的目录授权。
void Application::collect_tool_registration() noexcept {
    if (!tool_registration_) return;
    const auto status = conversation_.status();
    const auto instance = tool_registration_->instance.lock();
    if (!status || status->turn != tool_registration_->turn || status->exit != runtime::OperationState::pending ||
        !instance || instance->phase() != plugins::InstancePhase::active) tool_registration_.reset();
}

// 从现有运行推进验证并关闭无法验证的任务。
// 步骤：匹配原回合和实例 → 读取实际工具证据 → 保存判定并按结果请求停止或释放证明状态。
void Application::advance_verification() {
    if (!verifier_) return;
    const auto& run = conversation_.current_;
    if (!run || run->run_id() != verification_->run || !tool_registration_) verifier_->unavailable();
    else {
        try { verifier_->advance(session_,*run,caller_); }
        catch (...) { verifier_->unavailable(); }
    }
    if (const auto match = verifier_->match()) {
        verification_->match = match;
        if (*match != agent::ObservationMatch::satisfied && run && run->run_id() == verification_->run)
            run->request_cancel(runtime::CancelReason::owner_stop);
        verifier_.reset();
    }
}

// 清理失效的工具范围后推进插件的异步通知与收尾。
runtime::RunResult Application::advance_plugins() {
    collect_tool_registration();
    advance_verification();
    return plugins_.advance();
}

// 确认会话已归还实际工作，再交给原管理器分步关闭。
plugins::ShutdownResult Application::close_plugins() noexcept {
    if (!conversation_.closed()) return plugins::ShutdownResult::busy;
    tool_registration_.reset();
    plugins_.close();
    return plugins_.close_status();
}
}
