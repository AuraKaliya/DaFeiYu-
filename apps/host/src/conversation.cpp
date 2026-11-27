#include "conversation.hpp"
#include "persona.hpp"
#include "main_messages.hpp"
#include <dafeiyu/digest.hpp>
#include <dafeiyu/plugin_hooks.hpp>
#include <algorithm>
#include <random>

namespace dfy::app {
using runtime::OperationState;
namespace {
constexpr std::size_t display_turn_limit = 16;
}

// 接纳问答并固定唯一回合，拒绝覆盖尚待退出的工作。
// 步骤：核对当前回合 → 合并宿主控制 → 预留显示条目 → 通过原会话接纳并关联身份。
agent::TurnDiagnostics Conversation::submit(const std::string& prompt, agent::RunOptions options, bool player_facing,
    const std::string* display_prompt, DialogueAudience audience) {
    if (current_) throw std::logic_error("session: wait for the current request to finish");
    if (version_labels_.application_commit) options.version_labels.application_commit = version_labels_.application_commit;
    if (version_labels_.build) options.version_labels.build = version_labels_.build;
    if (version_labels_.model_profile) options.version_labels.model_profile = version_labels_.model_profile;
    const bool natural_dialogue = player_facing && !options.tools && !options.planning;
    if (player_facing && display_prompt && room_feed_) options.decision_observations = room_feed_;
    agent::DecisionStatusOptions main_status{"main",{},natural_dialogue,{},true};
    if (options.decision_status) main_status.sample = std::move(options.decision_status->sample);
    options.decision_status = std::move(main_status);
    const bool can_speak = player_facing && main_messages_ && main_messages_->prepare_output(options);
    if (main_messages_) main_messages_->prepare_memory(options,audience,prompt);
    if (persona_) {
        const auto route = can_speak ? agent::OutputRoute::main_chat_with_reply :
            player_facing ? agent::OutputRoute::main_chat : agent::OutputRoute::main_internal;
        persona_->apply(options,route);
    } else if (!options.version_labels.prompt_template && !options.system_instructions.empty())
        options.version_labels.prompt_template = "sha256:"+platform::sha256(options.system_instructions);
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
    if (display_prompt) {
        options.accept_user_input = options.origin == agent::RunOrigin::user_request && !options.planning;
        display_.turns.push_back({{},*display_prompt});
        display_.turns.back().accepts_input = options.accept_user_input;
    }
    try { current_ = session_.begin(prompt,std::move(options)); }
    catch (...) {
        if (display_prompt) display_.turns.pop_back();
        throw;
    }
    reported_ = false;
    user_output_ = player_facing;
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
    if (main_messages_) main_messages_->clear_context(last_ ? last_->turn : 0);
    if (!display_.turns.empty()) {
        display_.turns.clear();
        display_bytes_ = 0;
        ++display_.revision;
    }
    return receipt;
}

// 回收最早显示条目，保留最新问答和原身份。
void Conversation::trim_display() noexcept {
    while (display_.turns.size() > display_turn_limit || display_bytes_ > dialogue_display_byte_limit) {
        const auto& oldest = display_.turns.front();
        display_bytes_ -= oldest.prompt.size()+oldest.answer.size();
        for (const auto& input : oldest.inputs) display_bytes_ -= input.text.size();
        display_.turns.erase(display_.turns.begin());
    }
}

// 将用户回合的唯一结算与实际退出分别写入显示记录。
// 步骤：核对原回合身份 → 同步新收件与送模步骤 → 写入首次结果 → 刷新退出及显示修订。
void Conversation::update_display(const agent::TurnDiagnostics& status, const std::optional<CompletedTurn>& completed) {
    if (display_.turns.empty()) return;
    auto& turn = display_.turns.back();
    if (turn.run != agent::RunId{status.session,status.turn}) return;
    const bool input_changed = turn.input_revision != current_->input_revision();
    if (!completed && turn.exit == status.exit && !input_changed) return;
    if (input_changed) {
        auto inputs = current_->inputs();
        for (std::size_t i=turn.inputs.size(); i<inputs.size(); ++i) display_bytes_ += inputs[i].text.size();
        turn.inputs.swap(inputs);
        turn.input_revision = current_->input_revision();
    }
    if (completed) {
        turn.result = status.result;
        if ((!main_messages_ || !user_output_) && status.result == OperationState::succeeded && !completed->error) {
            turn.answer = completed->answer;
            display_bytes_ += turn.answer.size();
        }
    }
    turn.exit = status.exit;
    trim_display();
    ++display_.revision;
}

// 将已经入库的发言交给原回合的有界显示记录。
// 步骤：定位仍保留的回合 → 替换该回合的最近发言 → 刷新显示容量与修订。
void Conversation::published_reply(agent::RunId run, const std::string& text) {
    const auto turn = std::ranges::find(display_.turns,run,&DisplayedTurn::run);
    if (turn==display_.turns.end() || turn->answer==text) return;
    display_bytes_ -= turn->answer.size();
    turn->answer = text;
    display_bytes_ += text.size();
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
        completed.emplace(CompletedTurn{status,{},{},user_output_});
        if (status.result != OperationState::cancelled) {
            try { completed->answer = current_->answer(); }
            catch (...) { completed->error = std::current_exception(); }
        }
        reported_ = true;
    }

    update_display(status,completed);
    if (main_messages_) main_messages_->observe({status.session,status.turn},current_->inputs(),user_output_ && completed ? &*completed : nullptr);
    if (status.exit != OperationState::pending) {
        last_ = status;
        try { last_diagnostic_.emplace(capture_diagnostic()); diagnostic_error_ = {}; }
        catch (...) { last_diagnostic_.reset(); diagnostic_error_ = std::current_exception(); }
        current_.reset();
    }
    return completed;
}
}
