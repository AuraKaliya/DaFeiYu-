#include "application.hpp"
#include <dafeiyu/digest.hpp>
#include <dafeiyu/model_client.hpp>
#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>

namespace dfy::app {
namespace {
constexpr std::size_t dialogue_limit = 1024*1024;
// 固定应用的编译配置、已知源码提交及后端公开档案。
agent::RunVersionLabels build_labels(std::optional<std::string> model_profile) {
    agent::RunVersionLabels labels;
    labels.build = DFY_APPLICATION_BUILD;
    if (std::string_view(DFY_APPLICATION_COMMIT).size()) labels.application_commit = DFY_APPLICATION_COMMIT;
    labels.model_profile = std::move(model_profile);
    return labels;
}

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

// 用同一宿主身份装配信箱，并让正式会话与插件共享有界事件通道。
// 步骤：固定工具时钟与通信资源 → 创建插件目录与会话 → 绑定会话服务及公开构建标签。
Application::Application(model::Config config, const std::filesystem::path& packages, const std::filesystem::path& user_data,
    model::Backend backend, ApplicationRunConfig run)
    : tool_clock_(std::move(run.tool_clock)),
      channel_(std::make_shared<events::BroadcastChannel>(std::make_shared<events::EventStore>(256,64),16,8)),
      mailbox_host_{std::make_shared<events::Mailboxes>(events::MailboxLimits{36,16,128,16*1024}),runtime::next_domain()},
      dialogue_sender_(runtime::next_domain()),
      dialogue_mailbox_(mailbox_host_.network->open(mailbox_host_.identity,dialogue_sender_,dialogue_limit)),
      room_sender_(runtime::next_domain()), room_mailbox_(mailbox_host_.network->open(mailbox_host_.identity,room_sender_)),
      plugins_(packages,user_data,channel_,16,mailbox_host_), model_config_(std::move(config)), model_backend_(std::move(backend)),
      labels_(build_labels(std::move(run.model_profile))), session_(model_config_,channel_,model_backend_),
      conversation_(session_,labels_,&plugins_),
      mailbox_intake_(mailbox_host_,conversation_),
      subagents_(std::make_shared<Subagents>(plugins_,model_config_,channel_,model_backend_,labels_,tool_clock_)),
      wake_for_companion_confirmation_(run.wake_for_companion_confirmation) {
    if (!tool_clock_) throw std::invalid_argument("application: tool clock required");
    plugins_.enable_agents(subagents_);
    const events::MailboxCategory enabled[] = {{"room.addressed",1}};
    mailbox_intake_.watch(room_mailbox_,run.wake_for_room_address ? std::span<const events::MailboxCategory>(enabled) :
        std::span<const events::MailboxCategory>{},[](const auto& item) -> std::optional<std::string> {
            if (item.event.payload->descriptor()->name() != "room.addressed") return {};
            return std::string(item.event.payload->find("text")->as_string());
        });
}

// 在独立地址上交付受信终端原话，普通问答沿用单回合接纳。
// 步骤：核对主会话空闲 → 投递原正文并取得接纳序号 → 从本体信箱读回并接纳问答。
agent::TurnDiagnostics Application::submit_dialogue(const std::string& text) {
    if (conversation_.busy() || !conversation_.accepting()) throw std::logic_error("session: wait for the current request to finish");
    if (dialogue_sequence_ == UINT64_MAX) throw std::overflow_error("dialogue: mailbox sequence exhausted");
    auto& mail = *mailbox_host_.network;
    const auto receipt = mail.deliver(dialogue_sender_,dialogue_mailbox_,dialogue_sequence_+1,
        events::mailbox_text("dialogue.text",1,text,dialogue_limit));
    if (receipt.state != events::DeliveryState::accepted) throw std::logic_error("dialogue: unexpected acceptance state");
    ++dialogue_sequence_;
    const auto received = mail.read(mailbox_host_.identity,dialogue_mailbox_);
    return conversation_.submit(std::string(received.event.get().payload->find("text")->as_string()));
}

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
    options.now = tool_clock_;
    options.tools = prepared.catalog->snapshot(std::span(&tool_id,1),confirm_changes);
    options.policy.confirm_changes = confirm_changes;
    options.confirmation_caller = caller_;

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
    options.now = tool_clock_;
    const std::array ids{reader,change};
    options.tools = prepared.catalog->snapshot(ids,true);
    options.planning = agent::PlanLimits{1,1};
    options.confirmation_caller = caller_;
    options.policy.confirm_changes = true;
    options.policy.tool_limit = 3;
    options.confirmation_limit = 1;
    options.version_labels.prompt_template = "app.verified-state-v1";
    const auto now = options.now();
    const auto duration = std::chrono::duration_cast<agent::ToolTime::duration>(options.timeout);
    if (now > agent::ToolTime::max()-duration) throw std::overflow_error("verification: deadline overflow");
    const auto deadline = now+duration;
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

// 安全重复提交受控任务，不因丢失回执创建第二次动作。
// 步骤：校验请求及范围 → 核对原回执或顺序 → 完整准备摘要 → 接纳一次并发布原回执。
TaskAdmission Application::submit_verified_once(std::uint64_t command, const std::string& reader, const std::string& change,
    const std::string& expected, const std::string& prompt) {
    if (!command) throw std::invalid_argument("task: positive request required");
    auto& slot = task_receipts_[(command-1)%task_receipts_.size()];
    if (command <= task_watermark_ && (!slot || slot->command != command)) throw std::invalid_argument("task: request receipt expired");
    if (command > task_watermark_ && command-task_watermark_ != 1) throw std::invalid_argument("task: request sequence gap");
    if (reader.size() > 128 || change.size() > 128 || expected.size() > 1024 || prompt.size() > 1024*1024)
        throw std::length_error("task: command exceeds input limits");

    auto digest = platform::sha256(nlohmann::json::array({reader,change,expected,prompt}).dump());
    if (command <= task_watermark_) {
        if (slot->digest != digest) throw std::invalid_argument("task: request body conflicts with receipt");
        return {slot->accepted,true};
    }

    const auto accepted = submit_verified(reader,change,expected,prompt);
    slot.emplace(TaskReceipt{command,std::move(digest),accepted});
    task_watermark_ = command;
    return {accepted,false};
}

// 从有界回执取得原运行，缺失身份不解释成可以执行新任务。
std::optional<agent::RunId> Application::task_run(std::uint64_t command) const noexcept {
    if (!command) return {};
    const auto& slot = task_receipts_[(command-1)%task_receipts_.size()];
    if (!slot || slot->command != command) return {};
    return agent::RunId{slot->accepted.session,slot->accepted.turn};
}

// 在一次只读查询中返回同一运行的状态和验证判断。
// 步骤：读取状态并核对可查身份 → 匹配原读回记录 → 投影退出时的最终判断。
std::optional<RunStatusSnapshot> Application::run_status(std::optional<agent::RunId> expected) const noexcept {
    const auto status = conversation_.status();
    if (!status) return {};
    const agent::RunId run{status->session,status->turn};
    if (expected && *expected != run) return {};

    RunStatusSnapshot snapshot{*status,{}};
    if (verification_ && verification_->run == run) {
        snapshot.verification = verification_;
        if (!snapshot.verification->match && status->exit != runtime::OperationState::pending)
            snapshot.verification->match = agent::ObservationMatch::unknown;
    }
    return snapshot;
}

// 复用同一运行快照给出原读回判断。
std::optional<VerificationReport> Application::verification() const noexcept {
    const auto snapshot = run_status();
    return snapshot ? snapshot->verification : std::nullopt;
}

// 从原活动回合读取确认值。
std::vector<agent::ConfirmationSnapshot> Application::confirmations() const {
    if (conversation_.current_) return conversation_.current_->confirmations();
    return {};
}

// 由宿主固定审批对象，用户命令只提供请求、修订和决定。
// 步骤：复用原决定或拒绝冲突 → 复核实例及证据 → 交给原会话 → 保留值回执。
agent::ConfirmationReceipt Application::decide(runtime::DomainId request, std::uint64_t revision, bool approve) {
    if (!request || !revision) throw std::invalid_argument("confirmation: request and revision required");
    for (const auto& saved : decisions_) if (saved.receipt.request == request) {
        if (saved.expected != revision || saved.approve != approve) throw std::invalid_argument("confirmation: decision receipt conflict");
        return saved.receipt;
    }

    collect_tool_registration();
    advance_verification();
    const auto values = confirmations();
    const auto found = std::find_if(values.begin(),values.end(),[request](const auto& value) { return value.id == request; });
    if (found == values.end()) throw std::invalid_argument("confirmation: request unavailable");
    if (approve && found->state == agent::ConfirmationState::waiting && verification_ &&
        found->target.attempt.step.run == verification_->run &&
        (!verifier_ || !verifier_->can_approve(found->target,tool_clock_())))
        throw std::invalid_argument("verification: original read evidence is unavailable");

    const auto receipt = session_.decide(caller_,{request,request,revision,found->target,approve});
    decisions_[next_decision_] = {receipt,revision,approve};
    next_decision_ = (next_decision_+1)%decisions_.size();
    return receipt;
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
        try { verifier_->advance(session_,*run,caller_,tool_clock_()); }
        catch (...) { verifier_->unavailable(); }
    }
    if (const auto match = verifier_->match()) {
        verification_->match = match;
        if (*match != agent::ObservationMatch::satisfied && run && run->run_id() == verification_->run)
            run->request_cancel(runtime::CancelReason::owner_stop);
        verifier_.reset();
    }
}

// 按活动插件实例启用独立陪伴会话。
// 步骤：核对主入口和原会话 → 创建新绑定 → 发布宿主会话身份。
runtime::DomainId Application::open_companion(const std::string& package) {
    if (!session_.accepting()) throw std::logic_error("companion: application closing");
    const auto instance = plugins_.instance(package);
    if (instance->phase() != plugins::InstancePhase::active) throw std::invalid_argument("companion: inactive provider");
    if (companion_ && !companion_->closed()) {
        const auto original = companion_->status();
        if (!original.stopping && original.instance == instance->generation()) return original.session;
        throw std::logic_error("companion: previous session has not exited");
    }
    companion_ = std::make_unique<CompanionSession>(instance, model_config_, channel_, model_backend_, labels_, tool_clock_, mailbox_host_);
    return companion_->id();
}

// 将明确指定的用户意图投递到当前陪伴会话。
agent::TurnDiagnostics Application::submit_companion(runtime::DomainId session, const std::string& prompt) {
    if (!session_.accepting() || !companion_ || companion_->id() != session) throw std::invalid_argument("companion: stale session");
    return companion_->submit(prompt);
}

// 停止原游戏会话，旧界面不能停止替换后的实例。
void Application::stop_companion(runtime::DomainId session) {
    if (!companion_ || companion_->id() != session) throw std::invalid_argument("companion: stale session");
    companion_->request_stop();
}

// 复制最近的子会话状态，不推进模型或游戏。
std::optional<CompanionStatus> Application::companion_status() const {
    return companion_ ? std::optional(companion_->status()) : std::nullopt;
}

// 在同一个宿主推进节拍中请求原实例的状态。
std::uint64_t Application::refresh_companion(runtime::DomainId session) {
    if (!session_.accepting() || !companion_ || companion_->id() != session) throw std::invalid_argument("companion: stale session");
    return companion_->refresh();
}

// 返回当前可接收消息的角色地址。
std::optional<CompanionAddress> Application::companion_address() const {
    return companion_ ? companion_->address() : std::nullopt;
}
// 显式启用原会话绑定的游戏玩家入口。
void Application::listen_companion(runtime::DomainId session, CompanionMode mode) {
    if (!session_.accepting() || !companion_ || companion_->id() != session) throw std::invalid_argument("companion: stale session");
    companion_->listen(mode);
}
// 查询已验证的玩家身份及读取进度，不触发动作。
std::optional<PlayerInboxStatus> Application::companion_inbox(runtime::DomainId session) const {
    if (!companion_ || companion_->id() != session) return {};
    return companion_->inbox_status();
}
// 按原会话取得最近一次玩家查看委托。
std::optional<CompanionVisit> Application::companion_visit(runtime::DomainId session) const {
    return companion_ && companion_->id() == session ? companion_->visit() : std::nullopt;
}
// 在受信入口读取问题；旧会话不获得新问题的内容。
std::optional<CompanionQuestion> Application::companion_question(runtime::DomainId session) const {
    return companion_ && companion_->id() == session ? companion_->question() : std::nullopt;
}
// 接纳管理中心的原用户决定，旧会话与关闭中的应用都不能签发新委托。
CollectionGrant Application::authorize_companion(const CompanionAddress& address, std::uint64_t question,
    std::uint64_t revision, std::uint64_t command, std::uint64_t quantity) {
    if (!session_.accepting() || !companion_ || companion_->id() != address.session)
        throw std::invalid_argument("companion: stale management decision");
    return companion_->authorize(address, question, revision, command, quantity);
}
// 只读取得新委托、授权者和仍保留的原参数。
std::optional<CollectionGrant> Application::companion_collection(runtime::DomainId session) const {
    return companion_ && companion_->id() == session ? companion_->collection() : std::nullopt;
}
// 只有当前会话的投递状态可以返回给可信调用者。
std::optional<DeliveryStatus> Application::companion_delivery(runtime::DomainId session) const {
    return companion_ && companion_->id() == session ? std::optional(companion_->delivery()) : std::nullopt;
}
// 只读取原会话中的采掘进展。
std::optional<MiningStatus> Application::companion_mining(runtime::DomainId session) const {
    return companion_ && companion_->id() == session ? std::optional(companion_->mining()) : std::nullopt;
}
// 为当前游戏角色处理进度或会合问题，不与在途模型竞争。
LocationAnswer Application::locate_companion(runtime::DomainId session, LocationPurpose purpose) {
    if (!companion_ || companion_->id() != session) throw std::invalid_argument("companion: stale location query");
    if (!session_.accepting()) return {LocationBasis::disconnected, companion_->state().last, 0};
    return companion_->locate(purpose);
}
// 只在原陪伴会话中读取已记录的探索事实。
std::optional<ExplorationReview> Application::companion_review(runtime::DomainId session) const {
    return companion_ && companion_->id() == session ? companion_->review() : std::nullopt;
}
// 读取保留的原会话计划板，不发起世界重读、模型请求或游戏动作。
std::optional<CompanionBoard> Application::companion_board(runtime::DomainId session,
    std::optional<std::uint64_t> task, std::uint64_t after_message) const {
    if (!companion_ || companion_->id() != session) return {};
    return companion_->board(task, after_message);
}
// 由主侧向原会话发送业务消息，使用宿主绑定的方向。
std::uint64_t Application::send_companion(const CompanionAddress& address, CompanionMessageKind kind,
    std::uint64_t command, std::uint64_t reply_to, const std::string& text) {
    if (!session_.accepting() || !companion_ || companion_->id() != address.session) throw std::invalid_argument("companion: stale session");
    return companion_->send(address, kind, command, reply_to, text);
}
// 读取指定会话的业务历史，不将正文混入公开诊断。
CompanionDelta Application::companion_messages(runtime::DomainId id, std::uint64_t after) const {
    if (!companion_ || companion_->id() != id) throw std::invalid_argument("companion: stale session");
    return companion_->messages(after);
}

// 查询原会话的世界观察，旧会话不能读到替换后的状态。
std::optional<CompanionState> Application::companion_state(runtime::DomainId session) const {
    if (!companion_ || companion_->id() != session) return {};
    return companion_->state();
}

// 使用原会话和回合身份读取受信调用者需要的正文回执。
std::optional<CompletedTurn> Application::companion_result(agent::RunId run) const {
    if (!companion_ || companion_->id() != run.session) return {};
    return companion_->result(run.sequence);
}

// 在宿主线程上推进子会话和插件，主会话的调用方仍拥有其结算回执。
// 步骤：复核原工具范围 → 推进独立子会话 → 推进共享插件通知。
runtime::RunResult Application::advance_plugins() {
    collect_tool_registration();
    advance_verification();
    if (!conversation_.busy()) companion_exchange_tools_.reset();
    if (!session_.accepting()) subagents_->stop();
    subagents_->advance();
    if (companion_ && !companion_->closed()) {
        if (!session_.accepting()) companion_->close();
        companion_->advance();
    }
    const auto plugins = plugins_.advance();
    if (wake_for_companion_confirmation_) review_companion_confirmation();
    const auto mail = mailbox_intake_.advance();
    return mail == MailboxIntakeResult::rejected ? runtime::RunResult::failed : plugins;
}

// 等两段会话的工作真正退出后再关闭共享插件。
// 步骤：核对主会话收尾 → 关闭陪伴会话或等待 → 关闭原管理器。
plugins::ShutdownResult Application::close_plugins() noexcept {
    if (!conversation_.closed()) return plugins::ShutdownResult::busy;
    subagents_->stop();
    if (!subagents_->empty()) return plugins::ShutdownResult::pending;
    if (companion_ && !companion_->closed()) {
        try { companion_->close(); } catch (...) { return plugins::ShutdownResult::failed; }
        if (!companion_->closed()) return plugins::ShutdownResult::pending;
    }
    tool_registration_.reset();
    companion_exchange_tools_.reset();
    plugins_.close();
    return plugins_.close_status();
}
}
