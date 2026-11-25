#include "main_messages.hpp"
#include "application.hpp"
#include "run_log.hpp"
#include <dafeiyu/digest.hpp>
#include <dafeiyu/utf8.hpp>

namespace dfy::app {
using Json = nlohmann::json;
namespace {
const std::string reply_id = "main/reply";
// 保留实例、运行时会话与原回合的完整命名空间。
std::string reply_prefix(const std::string& instance, agent::RunId run) {
    return instance+"/"+std::to_string(run.session)+"/"+std::to_string(run.sequence)+"/";
}
}

// 绑定原后台的持久消息库与诊断，支持结构化调用的后端可在对话回合显式发言。
// 步骤：固定渠道所有者 → 注册面向当前主聊天的发言契约 → 绑定业务线程执行入口。
MainMessages::MainMessages(Application& application, std::shared_ptr<MessageStore> store, std::string instance,
    SoftwareLog* log, RunLog* trace, bool structured)
    : app_(application),store_(std::move(store)),instance_(std::move(instance)),log_(log),trace_(trace) {
    if (!structured) return;
    reply_tools_ = std::make_unique<agent::ToolCatalog>(1);
    reply_tools_->add({reply_id,app_.mailbox_host().identity,1,{"main_reply",
        "向当前主聊天发送一条发言或追问。text 为用户将看到的原文。回执给出投递号和写入状态。",
        R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"],"additionalProperties":false})"},
        true,true},[this](const agent::ToolInvocation& call) { return speak(call); });
}

// 为新对话重置防重范围，已有专用工具与原调用的参数授权仍由该入口持有。
// 步骤：归还上轮发言记录 → 核对入口与后端能力 → 固定工具及当轮说明。
void MainMessages::prepare_output(agent::RunOptions& options) {
    explicit_deliveries_.clear();
    auto state = std::make_shared<agent::ContextStatus>();
    state->binding = "main/"+instance_;
    if (delivered_) {
        const auto& saved = *delivered_;
        const auto& text = saved.envelope.at("text").get_ref<const std::string&>();
        state->fields = {{"delivery",saved.submission},{"channelMessage",std::to_string(saved.id)},
            {"turn",saved.envelope.at("turn").get<std::string>()},{"state","delivered"}};
        if (saved.envelope.contains("replyTo")) state->fields["replyTo"] = saved.envelope.at("replyTo").get<std::string>();
        if (text.size() <= 4096) state->fields["deliveredText"] = text;
        else {
            auto bytes = std::size_t{2048};
            while ((static_cast<unsigned char>(text[bytes]) & 0xc0) == 0x80) --bytes;
            state->fields["deliveredPrefix"] = text.substr(0,bytes);
            state->fields["originalBytes"] = std::to_string(text.size());
            state->fields["textSha256"] = platform::sha256(text);
        }
    }
    for (const auto& [key,envelope] : replies_) {
        if (std::stoull(envelope.at("turn").get<std::string>()) > context_floor_) {
            state->fields["pendingDelivery"] = key;
            break;
        }
    }
    if (!state->fields.empty()) options.context_status = std::move(state);
    if (!reply_tools_ || options.tools || options.planning) return;
    options.tools = reply_tools_->snapshot(std::span(&reply_id,1),true);
    options.system_instructions += " 普通最终文本会交给主聊天用户。需要提前发言或追问时可调用 main_reply。";
}

// 将一次写入与真实渠道消息号关联，Trace 只记录可复查的投递身份。
// 步骤：按原投递号写入 → 发布首次提交到原窗口与终端 → 关联诊断并返回原消息号。
std::int64_t MainMessages::commit_reply(const Json& envelope) {
    const auto& key = envelope.at("delivery").get_ref<const std::string&>();
    auto saved = store_->put("main",key,envelope,{{"state","delivered"}});
    const auto message_id = saved.message.id;
    const agent::RunId run{std::stoull(envelope.at("session").get<std::string>()),std::stoull(envelope.at("turn").get<std::string>())};
    if (run.sequence > context_floor_ && (!delivered_ || message_id > delivered_->id)) delivered_ = std::move(saved.message);
    if (!saved.created) return message_id;

    const auto& text = envelope.at("text").get_ref<const std::string&>();
    app_.conversation().published_reply(run,text);
    if (reply_sink_) reply_sink_(text);
    if (trace_) {
        Json event{{"session",run.session},{"run",run.sequence},{"delivery",key},{"message",std::to_string(message_id)}};
        if (envelope.contains("replyTo")) event["replyTo"] = envelope.at("replyTo");
        trace_->append("dialogue.reply_delivered",std::move(event));
    }
    return message_id;
}

// 所有面向用户的主会话发言沿同一持久写口取得回执。
// 步骤：绑定原回合与用户消息 → 提交一次渠道发言 → 失败时保留同号原件供补写。
Json MainMessages::reply(agent::RunId run, const std::string& suffix, const std::string& text) {
    const auto key = reply_prefix(instance_,run)+suffix;
    Json envelope{{"sender","assistant"},{"route","reply"},{"text",text},{"delivery",key},{"instance",instance_},
        {"session",std::to_string(run.session)},{"turn",std::to_string(run.sequence)}};
    if (parent_.first==run) envelope["replyTo"] = std::to_string(parent_.second);

    std::int64_t message;
    try { message = commit_reply(envelope); }
    catch (const std::runtime_error&) {
        if (replies_.try_emplace(key,std::move(envelope)).second) storage_failed();
        retry_at_ = std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
        return {{"state","pending"},{"delivery",key}};
    }
    return {{"state","delivered"},{"delivery",key},{"message",std::to_string(message)}};
}

// 返回原回合的一项待写投递，后续重试沿该身份完成。
std::string MainMessages::pending_reply(agent::RunId run) const {
    if (replies_.empty()) return {};
    const auto prefix = reply_prefix(instance_,run);
    const auto found = replies_.lower_bound(prefix);
    return found!=replies_.end() && found->first.starts_with(prefix) ? found->first : std::string{};
}

// 复用同一回合中同文显式发言的原投递，否则交付本轮独立的最终答复。
void MainMessages::final_reply(agent::RunId run, const std::string& text) {
    const auto spoken = explicit_deliveries_.find(text);
    if (spoken==explicit_deliveries_.end()) reply(run,"final",text);
    else if (trace_) trace_->append("dialogue.reply_reused",{{"session",run.session},{"run",run.sequence},{"delivery",spoken->second}});
}

// 在工具执行时按原调用登记发言，重复的最终文本复用该投递。
// 步骤：核对实际正文 → 由回合、步骤和调用号固定身份 → 返回原写入状态。
std::unique_ptr<agent::ToolWork> MainMessages::speak(const agent::ToolInvocation& invocation) {
    const auto text = Json::parse(invocation.call.arguments).at("text").get<std::string>();
    if (text.size()>dialogue_text_limit || text.find_first_not_of(" \t\r\n")==text.npos || !runtime::valid_utf8(text))
        return agent::completed_tool({false,agent::ToolEffect::not_applied,"invalid reply text",agent::ToolError::failed});
    const auto step = invocation.attempt.step;
    const auto receipt = reply(step.run,"tool-"+platform::sha256(std::to_string(step.sequence)+"/"+invocation.call.id),text);
    explicit_deliveries_.insert_or_assign(text,receipt.at("delivery").get<std::string>());
    const bool delivered = receipt.at("state")=="delivered";
    return agent::completed_tool({delivered,delivered ? agent::ToolEffect::applied : agent::ToolEffect::unknown,
        receipt.dump(),delivered ? agent::ToolError::none : agent::ToolError::unavailable});
}
}
