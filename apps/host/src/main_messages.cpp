#include "main_messages.hpp"
#include "application.hpp"
#include "software_log.hpp"
#include <thread>

namespace dfy::app {
using Json = nlohmann::json;
// 原始目标是幂等比较的一部分，来源时间只在渠道提供时写入。
Json user_envelope(std::string_view route, const std::string& text, const std::string& instance,
    agent::RunId target, std::optional<std::int64_t> source_utc_ms) {
    Json value{{"sender","user"},{"route",route},{"text",text}};
    if (target.session) value["target"] = {{"instance",instance},{"session",std::to_string(target.session)},{"turn",std::to_string(target.sequence)}};
    if (source_utc_ms) value["sourceUtcMs"] = *source_utc_ms;
    return value;
}
// 以原后台的身份记录失败，存储层错误与 Agent Trace 独立。
void MainMessages::storage_failed() const noexcept {
    if (log_) log_->record(SoftwareEvent::message_storage,SoftwareResult::failed);
}
// 已取得的回执可重试写盘，外部效果不能重放。
void MainMessages::publish(std::int64_t id, const Json& receipt) {
    try { store_->update(id,receipt); retry_.erase(id); }
    catch (const std::runtime_error&) {
        if (!retry_.contains(id)) storage_failed();
        retry_[id] = receipt;
        retry_at_ = std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
    }
}
// 在有限频率下补写积压的业务结果。
void MainMessages::flush() {
    if (std::chrono::steady_clock::now()<retry_at_) return;
    if (!pending_.empty() && app_.conversation().accepting()) {
        const auto& display = app_.conversation().display();
        if (pending_.begin()->second.envelope.at("route")!="send" || (!app_.conversation().busy() && (display.queue.empty() || display.queue_paused))) {
            auto item = pending_.extract(pending_.begin());
            deliver(item.key(),item.mapped().envelope,item.mapped().stored_utc_ms);
        }
    }
    if (!replies_.empty()) {
        const auto first = replies_.begin();
        try { commit_reply(first->second); replies_.erase(first); }
        catch (const std::runtime_error&) { retry_at_ = std::chrono::steady_clock::now()+std::chrono::milliseconds(250); }
    }
    if (retry_.empty()) return;
    const auto first = retry_.begin();
    const auto id = first->first;
    const auto receipt = first->second;
    publish(id,receipt);
}
// 先提交正在交付标记，再依据实际应用回执记录接纳、排队或原回合收件。
// 步骤：固定交付边界 → 核对原实例 → 调用对应入口 → 以同一消息号记录实际结果。
Json MainMessages::deliver(std::int64_t id, const Json& envelope, std::int64_t stored_utc_ms) {
    Json result{{"state","dispatching"},{"instance",instance_}};
    try { store_->update(id,result); }
    catch (const std::runtime_error&) {
        if (!pending_.contains(id)) storage_failed();
        pending_[id] = {envelope,stored_utc_ms};
        retry_at_ = std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
        return {{"state","pending"}};
    }
    const auto utc = envelope.value("sourceUtcMs",stored_utc_ms);
    auto& text = envelope.at("text").get_ref<const std::string&>();
    agent::RunId run{};
    std::uint64_t native = 0;
    try {
        if (envelope.at("route")=="send") {
            constexpr std::string_view revoke = "撤回记忆 ";
            if (text.starts_with(revoke)) change_memory(std::string_view(text).substr(revoke.size()),std::nullopt);
            if (memory_capture_topic(text)) incoming_memory_ = id;
            const auto start = app_.submit_dialogue(text,utc);
            run = {start.session,start.turn};
            parent_ = {run,id};
            result["state"] = "accepted";
        } else {
            const auto& target = envelope.at("target");
            run = {std::stoull(target.at("session").get<std::string>()),std::stoull(target.at("turn").get<std::string>())};
            if (target.at("instance")!=instance_) result["state"] = "stale";
            else if (envelope.at("route")=="append") {
                const auto received = app_.append_dialogue(run,text,utc);
                native = received.message;
                if (received.state==agent::InputAdmission::accepted) appended_[{run.sequence,native}] = id;
                using State = agent::InputAdmission;
                result["state"] = received.state==State::accepted ? "waiting" : received.state==State::closed ? "stale" :
                    received.state==State::full ? "full" : received.state==State::unavailable ? "unavailable" : "rejected";
            } else {
                const auto queued = app_.enqueue_dialogue(run,text,utc);
                native = queued.message;
                if (queued.state==QueueAdmission::accepted) queued_[native] = id;
                result["state"] = queued.state==QueueAdmission::accepted ? "queued" : queued.state==QueueAdmission::stale ? "stale" :
                    queued.state==QueueAdmission::full ? "full" : "rejected";
            }
        }
    } catch (const std::logic_error&) { result["state"] = "rejected"; }
    catch (const std::exception&) { result["state"] = "unknown"; }
    incoming_memory_.reset();
    result.update(Json{{"session",std::to_string(run.session)},{"turn",std::to_string(run.sequence)},{"nativeMessage",std::to_string(native)}});
    publish(id,result);
    return result;
}
// 原始命令身份在超时重查时保持不变，旧投递结果不启动新回合。
void MainMessages::terminal(const std::string& submission, const std::string& text) {
    const auto envelope = user_envelope("send",text,{}, {});
    if (const auto previous = store_->find("terminal",submission)) {
        if (previous->envelope!=envelope) throw std::invalid_argument("messages: submission conflict");
        if (previous->receipt.at("state")=="accepted") return;
        throw std::runtime_error("dialogue: "+previous->receipt.at("state").get<std::string>());
    }
    if (!app_.conversation().accepting() || app_.conversation().busy()) throw std::logic_error("session: wait for the current request to finish");
    const auto& queue = app_.conversation().display();
    if (!queue.queue.empty() && !queue.queue_paused) throw std::logic_error("dialogue: next message already queued");
    const auto accepted = store_->put("terminal",submission,envelope,{{"state","pending"}});
    const auto receipt = accepted.created ? deliver(accepted.message.id,envelope,accepted.message.utc_ms) : accepted.message.receipt;
    if (receipt.at("state")!="accepted") throw std::runtime_error("dialogue: "+receipt.at("state").get<std::string>());
}

// 只对可证明尚未开始的发送继续投递，原回合的未知实效留给原身份查证。
// 步骤：分批读取待办 → 重建原队列并暂停 → 恢复未交付普通输入 → 结算旧目标或标记待核对。
void MainMessages::restore() {
    std::int64_t after = 0;
    for (;;) {
        const auto rows = store_->unfinished(after);
        if (rows.empty()) return;
        for (const auto& row : rows) {
            after = row.id;
            if (row.envelope.value("sender",std::string{})!="user") continue;
            const auto& state = row.receipt.at("state");
            if (state=="pending" && row.envelope.at("route")=="send") {
                pending_.emplace(row.id,PendingInput{row.envelope,row.utc_ms});
                continue;
            }
            auto receipt = row.receipt;
            if (state=="queued") {
                const auto native = app_.restore_queued_dialogue(row.envelope.at("text").get_ref<const std::string&>(),
                    row.envelope.value("sourceUtcMs",row.utc_ms));
                queued_[native] = row.id;
                receipt.update(Json{{"instance",instance_},{"session",std::to_string(app_.conversation().id())},
                    {"turn","0"},{"nativeMessage",std::to_string(native)},{"paused",true}});
            } else receipt["state"] = state=="pending" ? "stale" : "unknown";
            store_->update(row.id,receipt);
        }
    }
}

// 只等待已经发生的写盘责任，尚未执行的原话已在待办表中。
void MainMessages::finish() {
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(4);
    while (!retry_.empty() || !replies_.empty()) {
        flush();
        if (std::chrono::steady_clock::now()>=end) throw std::runtime_error("messages: verified results could not be saved");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// 队首的下一轮可能已经发生外部效果，必须在启动前提交这个边界。
void MainMessages::start_queue(std::uint64_t native) {
    if (const auto it = queued_.find(native); it!=queued_.end()) {
        store_->update(it->second,{{"state","dispatching"},{"instance",instance_},{"nativeMessage",std::to_string(native)}});
        retry_.erase(it->second);
    }
}
// 归还等待项，并将后续回答关联到原入队消息。
void MainMessages::started_queue(std::uint64_t native, agent::RunId run) {
    if (auto item = queued_.extract(native); !item.empty()) {
        parent_ = {run,item.mapped()};
        publish(item.mapped(),{{"state","accepted"},{"instance",instance_},{"session",std::to_string(run.session)},
            {"turn",std::to_string(run.sequence)},{"nativeMessage",std::to_string(native)}});
    }
}
// 先写移除回执再归还队列映射，存储异常时保留原项。
void MainMessages::remove_queue(std::uint64_t native) {
    if (const auto it = queued_.find(native); it!=queued_.end()) {
        store_->update(it->second,{{"state","removed"},{"instance",instance_},{"nativeMessage",std::to_string(native)}});
        retry_.erase(it->second);
        queued_.erase(it);
    }
}
// 从会话的原始收件状态读取实际提交步骤，显示窗口裁剪不影响该事实。
// 步骤：按已送模的补充更新回复来源 → 写入已确定的收件结算 → 按原回合交付最终文本。
void MainMessages::observe(agent::RunId run, const std::vector<agent::TurnInput>& inputs, const CompletedTurn* completed) {
    for (const auto& input : inputs) {
        if (input.state==agent::InputState::waiting) continue;
        if (input.state == agent::InputState::submitted && input.message > output_input_) {
            output_input_ = input.message;
            explicit_deliveries_.clear();
        }
        if (auto item = appended_.extract(std::pair{run.sequence,input.message}); !item.empty()) {
            if (input.state == agent::InputState::submitted) parent_ = {run,item.mapped()};
            publish(item.mapped(),{{"state",agent::input_state_name(input.state)},{"instance",instance_},{"session",std::to_string(run.session)},
                {"turn",std::to_string(run.sequence)},{"nativeMessage",std::to_string(input.message)},{"modelStep",std::to_string(input.model_step)}});
        }
    }
    if (!completed || completed->error || completed->status.result!=runtime::OperationState::succeeded ||
        completed->answer.find_first_not_of(" \t\r\n")==std::string::npos) return;
    final_reply(run,completed->answer);
}
}
