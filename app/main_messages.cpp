#include "main_messages.hpp"
#include "application.hpp"
#include "software_log.hpp"

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
    if (retry_.empty() || std::chrono::steady_clock::now()<retry_at_) return;
    const auto first = retry_.begin();
    const auto id = first->first;
    const auto receipt = first->second;
    publish(id,receipt);
}
// 先提交正在交付标记，再依据实际应用回执记录接纳、排队或原回合收件。
// 步骤：固定交付边界 → 核对原实例 → 调用对应入口 → 以同一消息号记录实际结果。
Json MainMessages::deliver(std::int64_t id, const Json& envelope) {
    Json result{{"state","dispatching"},{"instance",instance_}};
    store_->update(id,result);
    auto& text = envelope.at("text").get_ref<const std::string&>();
    agent::RunId run{};
    std::uint64_t native = 0;
    try {
        if (envelope.at("route")=="send") {
            const auto start = app_.submit_dialogue(text);
            run = {start.session,start.turn};
            result["state"] = "accepted";
        } else {
            const auto& target = envelope.at("target");
            run = {std::stoull(target.at("session").get<std::string>()),std::stoull(target.at("turn").get<std::string>())};
            if (target.at("instance")!=instance_) result["state"] = "stale";
            else if (envelope.at("route")=="append") {
                const auto received = app_.append_dialogue(run,text);
                native = received.message;
                using State = agent::InputAdmission;
                result["state"] = received.state==State::accepted ? "waiting" : received.state==State::closed ? "stale" :
                    received.state==State::full ? "full" : received.state==State::unavailable ? "unavailable" : "rejected";
            } else {
                const auto queued = app_.enqueue_dialogue(run,text);
                native = queued.message;
                result["state"] = queued.state==QueueAdmission::accepted ? "queued" : queued.state==QueueAdmission::stale ? "stale" :
                    queued.state==QueueAdmission::full ? "full" : "rejected";
            }
        }
    } catch (const std::logic_error&) { result["state"] = "rejected"; }
    catch (const std::exception&) { result["state"] = "unknown"; }
    result.update(Json{{"session",std::to_string(run.session)},{"turn",std::to_string(run.sequence)},{"nativeMessage",std::to_string(native)}});
    publish(id,result);
    return result;
}
// 原始命令身份在超时重查时保持不变，旧投递结果不启动新回合。
void MainMessages::terminal(const std::string& submission, const std::string& text) {
    const auto envelope = user_envelope("send",text,{}, {});
    if (!app_.conversation().accepting() || app_.conversation().busy()) throw std::logic_error("session: wait for the current request to finish");
    const auto accepted = store_->put("terminal",submission,envelope,{{"state","pending"}});
    const auto receipt = accepted.created ? deliver(accepted.message.id,envelope) : accepted.message.receipt;
    if (receipt.at("state")!="accepted") throw std::runtime_error("dialogue: "+receipt.at("state").get<std::string>());
}
}
