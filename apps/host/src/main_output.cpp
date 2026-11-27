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
    : app_(application),store_(std::move(store)),instance_(std::move(instance)),
      memory_revision_(store_->memory_revision()),log_(log),trace_(trace) {
    if (!structured) return;
    reply_tools_ = std::make_unique<agent::ToolCatalog>(3);
    reply_tools_->add({reply_id,app_.mailbox_host().identity,1,{"main_reply",
        "向当前主聊天发送一条发言或追问。text 为用户将看到的原文。回执给出投递号和写入状态。",
        R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"],"additionalProperties":false})"},
        true,true,{},{},0,1024,true},[this](const agent::ToolInvocation& call) { return speak(call); });
    register_memory_tools();
}

// 冻结最近实收原件和仍待落盘的投递，超长正文保留可按原渠道定位的前缀。
// 步骤：登记渠道拥有的结构 → 沿原消息与提交号取样 → 输出本步显式状态。
agent::DecisionStatusSource MainMessages::delivery_status() const {
    static const auto definitions = [] {
        const Json text{{"type","string"}};
        Json fields = Json::object();
        for (const auto key : {"delivery","channelMessage","turn","state","deliveredUtcMs","replyTo","deliveredText",
            "deliveredPrefix","originalBytes","textSha256"}) fields[key] = text;
        const Json receipt{{"type","object"},{"properties",fields},{"required",{"delivery","channelMessage","turn","state","deliveredUtcMs"}},
            {"additionalProperties",false}};
        const Json pending{{"type","object"},{"properties",{{"pendingDelivery",text}}},{"required",{"pendingDelivery"}},{"additionalProperties",false}};
        return Json::array({
            {{"key","host.delivery"},{"version",1},{"description","最近实际写入主聊天的发言与原渠道消息"},{"defaultAttention",{{"mode","explicit"}}},{"valueSchema",receipt}},
            {{"key","host.pending_delivery"},{"version",1},{"description","仍待原号写入的主聊天发言"},{"defaultAttention",{{"mode","explicit"}}},{"valueSchema",pending}}
        }).dump();
    }();
    auto samples = Json::array();
    if (delivered_) {
        const auto& saved = *delivered_;
        const auto& text = saved.envelope.at("text").get_ref<const std::string&>();
        Json value = {{"delivery",saved.submission},{"channelMessage",std::to_string(saved.id)},
            {"turn",saved.envelope.at("turn").get<std::string>()},{"state","delivered"},{"deliveredUtcMs",std::to_string(saved.utc_ms)}};
        if (saved.envelope.contains("replyTo")) value["replyTo"] = saved.envelope.at("replyTo").get<std::string>();
        if (text.size() <= 4096) value["deliveredText"] = text;
        else {
            auto bytes = std::size_t{2048};
            while ((static_cast<unsigned char>(text[bytes]) & 0xc0) == 0x80) --bytes;
            value["deliveredPrefix"] = text.substr(0,bytes);
            value["originalBytes"] = std::to_string(text.size());
            value["textSha256"] = platform::sha256(text);
        }
        samples.push_back({{"key","host.delivery"},{"version",1},{"subject","main"},{"fact",saved.id},
            {"sampledUtcMs",saved.utc_ms},{"value",std::move(value)}});
    }
    for (const auto& [key,envelope] : replies_) {
        const auto turn = std::stoull(envelope.at("turn").get<std::string>());
        if (turn > context_floor_) {
            samples.push_back({{"key","host.pending_delivery"},{"version",1},{"subject",key},{"fact",turn},
                {"value",{{"pendingDelivery",key}}}});
            break;
        }
    }
    return {"host",definitions,samples.dump()};
}

// 为本轮主聊天绑定原渠道状态读口及发言权限；工具续行重新取得实际回执。
// 步骤：重置原轮输出 → 绑定业务线程源 → 对获准的入口提供发言工具。
bool MainMessages::prepare_output(agent::RunOptions& options, DialogueAudience audience, const std::string& prompt) {
    explicit_deliveries_.clear();
    output_input_ = 0;
    options.decision_status->sample = [source=weak_from_this()] {
        const auto channel = source.lock();
        if (!channel) throw std::logic_error("dialogue: status source closed");
        return std::vector{channel->delivery_status()};
    };
    if (!reply_tools_ || options.tools || options.planning) return false;
    if (audience==DialogueAudience::owner && !memory_capture_topic(prompt) &&
        !prompt.starts_with("回想 ") && !prompt.starts_with("撤回记忆 ")) {
        const std::string ids[]{reply_id,"main/memory_note","main/memory_search"};
        options.tools = reply_tools_->snapshot(ids,true);
    } else options.tools = reply_tools_->snapshot(std::span(&reply_id,1),true);
    return true;
}

// 将一次写入与真实渠道消息号关联，Trace 只记录可复查的投递身份。
// 步骤：按原投递号写入 → 发布首次提交到原窗口与终端 → 关联诊断并返回原消息号。
std::int64_t MainMessages::commit_reply(const Json& envelope) {
    const auto& key = envelope.at("delivery").get_ref<const std::string&>();
    auto saved = store_->put("main",key,envelope,{{"state","delivered"}});
    const auto message_id = saved.message.id;
    const agent::RunId run{std::stoull(envelope.at("session").get<std::string>()),std::stoull(envelope.at("turn").get<std::string>())};
    if (run.sequence > context_floor_ && (!delivered_ || message_id > delivered_->id)) delivered_ = std::move(saved.message);
    const auto& text = envelope.at("text").get_ref<const std::string&>();
    const auto spoken = explicit_deliveries_.find(text);
    if (spoken != explicit_deliveries_.end() && spoken->second.at("delivery") == key)
        spoken->second = {{"state","delivered"},{"delivery",key},{"message",std::to_string(message_id)}};
    if (!saved.created) return message_id;

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
    else if (trace_) trace_->append("dialogue.reply_reused",{{"session",run.session},{"run",run.sequence},{"delivery",spoken->second.at("delivery")}});
}

// 同一来源下重复提出的发言沿第一次投递查回执。
// 步骤：核对实际正文 → 复用原回执或按首次调用投递 → 返回真实写入状态。
std::unique_ptr<agent::ToolWork> MainMessages::speak(const agent::ToolInvocation& invocation) {
    const auto text = Json::parse(invocation.call.arguments).at("text").get<std::string>();
    if (text.size()>dialogue_text_limit || text.find_first_not_of(" \t\r\n")==text.npos || !runtime::valid_utf8(text))
        return agent::completed_tool({false,agent::ToolEffect::not_applied,"invalid reply text",agent::ToolError::failed});
    auto spoken = explicit_deliveries_.find(text);
    if (spoken == explicit_deliveries_.end()) {
        const auto step = invocation.attempt.step;
        auto receipt = reply(step.run,"tool-"+platform::sha256(std::to_string(step.sequence)+"/"+invocation.call.id),text);
        spoken = explicit_deliveries_.emplace(text,std::move(receipt)).first;
    }
    const auto& receipt = spoken->second;
    const bool delivered = receipt.at("state")=="delivered";
    return agent::completed_tool({delivered,delivered ? agent::ToolEffect::applied : agent::ToolEffect::unknown,
        receipt.dump(),delivered ? agent::ToolError::none : agent::ToolError::unavailable});
}
}
