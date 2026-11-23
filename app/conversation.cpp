#include "conversation.hpp"
#include "persona.hpp"
#include <dafeiyu/digest.hpp>
#include <dafeiyu/plugin_hooks.hpp>
#include <random>

namespace dfy::app {
using runtime::OperationState;
namespace {
constexpr std::size_t display_turn_limit = 16;
// 单轮有效正文受应用入口和 Session 已提交历史的容量约束。
constexpr std::size_t display_byte_limit = 2*1024*1024;
}

// 接纳问答并固定唯一回合，拒绝覆盖尚待退出的工作。
// 步骤：核对当前回合 → 合并宿主控制 → 预留显示条目 → 通过原会话接纳并关联身份。
agent::TurnDiagnostics Conversation::submit(const std::string& prompt, agent::RunOptions options, bool player_facing,
    const std::string* display_prompt) {
    if (current_) throw std::logic_error("session: wait for the current request to finish");
    if (version_labels_.application_commit) options.version_labels.application_commit = version_labels_.application_commit;
    if (version_labels_.build) options.version_labels.build = version_labels_.build;
    if (version_labels_.model_profile) options.version_labels.model_profile = version_labels_.model_profile;
    if (!options.version_labels.prompt_template && !options.system_instructions.empty())
        options.version_labels.prompt_template = "sha256:"+platform::sha256(options.system_instructions);
    if (player_facing && persona_) persona_->apply(options);
    if (plugins_) {
        const auto snapshot = plugins_->capture_hooks();
        auto version = snapshot.version;
        auto host = plugins::plugin_controls(snapshot);
        options.text_hook = plugins::plugin_text_hooks(snapshot);
        options.exit_observer = plugins::plugin_exit_hook(snapshot);
        if (options.extension) {
            if (options.version_labels.control_extension && version != "none")
                options.version_labels.control_extension = "sha256:"+platform::sha256(version+"/"+*options.version_labels.control_extension);
            // 保留调用者已固定的基础策略，插件只能使用宿主规则收紧本轮边界。
            options.extension = [host = std::move(host), local = std::move(options.extension)](auto point, const auto& context) {
                return agent::restrict_policy(host(point,context),local,point,context);
            };
        } else {
            options.version_labels.control_extension = std::move(version);
            options.extension = std::move(host);
        }
    }
    if (display_prompt) display_.turns.push_back({{},*display_prompt});
    try { current_ = session_.begin(prompt,std::move(options)); }
    catch (...) {
        if (display_prompt) display_.turns.pop_back();
        throw;
    }
    reported_ = false;
    const auto accepted = current_->diagnostics();
    if (display_prompt) {
        auto& turn = display_.turns.back();
        turn.run = {accepted.session,accepted.turn};
        display_bytes_ += turn.prompt.size();
        trim_display();
        ++display_.revision;
    }
    return accepted;
}

// 仅向当前待结算回合请求协作取消。
bool Conversation::cancel() {
    if (!current_ || current_->operation()->state() != OperationState::pending) return false;
    current_->request_cancel(runtime::CancelReason::user);
    return true;
}

// 原会话确认清空后收回本进程可见正文，同时保留原通知回执。
events::ObservationReceipt Conversation::clear() {
    const auto receipt = session_.clear();
    if (!display_.turns.empty()) {
        display_.turns.clear();
        display_bytes_ = 0;
        ++display_.revision;
    }
    return receipt;
}

// 回收最早显示条目，保留最新问答和原身份。
void Conversation::trim_display() noexcept {
    while (display_.turns.size() > display_turn_limit || display_bytes_ > display_byte_limit) {
        const auto& oldest = display_.turns.front();
        display_bytes_ -= oldest.prompt.size()+oldest.answer.size();
        display_.turns.erase(display_.turns.begin());
    }
}

// 将用户回合的唯一结算与实际退出分别写入显示记录。
// 步骤：核对原回合身份 → 写入首次结果及成功答复 → 刷新退出与显示修订。
void Conversation::update_display(const agent::TurnDiagnostics& status, const std::optional<CompletedTurn>& completed) {
    if (display_.turns.empty()) return;
    auto& turn = display_.turns.back();
    if (turn.run != agent::RunId{status.session,status.turn} || (!completed && turn.exit == status.exit)) return;
    if (completed) {
        turn.result = status.result;
        if (status.result == OperationState::succeeded && !completed->error) {
            turn.answer = completed->answer;
            display_bytes_ += turn.answer.size();
        }
    }
    turn.exit = status.exit;
    trim_display();
    ++display_.revision;
}

// 返回当前回合或最近一次实际退出的诊断值。
std::optional<agent::TurnDiagnostics> Conversation::status() const noexcept {
    if (current_) return current_->diagnostics();
    return last_;
}

// 宿主生成跨进程可区分的会话命名空间。
// 步骤：首个诊断时准备独立随机标识 → 复用同一会话标识 → 投影原回合。
agent::DiagnosticSnapshot Conversation::capture_diagnostic() const {
    if (diagnostic_namespace_.empty()) {
        std::random_device source;
        std::string name = "app";
        for (unsigned i = 0; i < 4; ++i) name += "-"+std::to_string(source());
        diagnostic_namespace_ = std::move(name);
    }
    return agent::diagnostic_snapshot(*current_,diagnostic_namespace_);
}

// 查询当前或最后一次诊断，失败按本轮错误独立报告。
std::optional<agent::DiagnosticSnapshot> Conversation::diagnostic() const {
    if (current_) return capture_diagnostic();
    if (diagnostic_error_) std::rethrow_exception(diagnostic_error_);
    return last_diagnostic_;
}

// 返回一次业务结算，同时继续跟踪实际退出。
// 步骤：推进原会话 → 提取首次结算的结果或错误 → 实际退出后保存状态及诊断值并归还回合。
std::optional<CompletedTurn> Conversation::advance() {
    session_.advance();
    if (!current_) return {};

    const auto status = current_->diagnostics();
    std::optional<CompletedTurn> completed;
    if (!reported_ && status.result != OperationState::pending) {
        completed.emplace(CompletedTurn{status,{},{}});
        if (status.result != OperationState::cancelled) {
            try { completed->answer = current_->answer(); }
            catch (...) { completed->error = std::current_exception(); }
        }
        reported_ = true;
    }

    update_display(status,completed);
    if (status.exit != OperationState::pending) {
        last_ = status;
        try { last_diagnostic_.emplace(capture_diagnostic()); diagnostic_error_ = {}; }
        catch (...) { last_diagnostic_.reset(); diagnostic_error_ = std::current_exception(); }
        current_.reset();
    }
    return completed;
}
}
