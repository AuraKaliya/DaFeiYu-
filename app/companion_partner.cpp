#include "companion.hpp"
#include <nlohmann/json.hpp>

namespace dfy::app {
namespace {
using Json = nlohmann::json;
// 按游戏聊天框的 UTF-16 长度取完整字符，避免换行触发多条游戏命令。
std::string chat_line(const std::string& text) {
    std::string line;
    std::size_t units = 0;
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = text[i];
        const std::size_t bytes = c < 128 ? 1 : c < 224 ? 2 : c < 240 ? 3 : 4;
        if (units + (bytes == 4 ? 2 : 1) > 214) { line += "…"; break; }
        if (c < 32) line += ' ';
        else line.append(text, i, bytes);
        i += bytes;
        units += bytes == 4 ? 2 : 1;
    }
    return line;
}
// 生成只交给插件实现的原始玩家消息身份。
std::string source_context(const CompanionAddress& address, std::uint64_t order) {
    return Json{{"generation",address.world},{"order",order}}.dump();
}
}

// 通过和模型相同的已注册回复能力投递最终文本，保持原世界和原玩家绑定。
void CompanionSession::start_partner_reply(const std::string& text) {
    auto& order = *partner_order_;
    order.reply = chat_line(text);
    const auto& route = partner_reply_tool_;
    auto scope = catalog_->snapshot(std::span(&route, 1), true).bind_context(source_context(order.address, order.order));
    const auto name = scope.definitions().at(0).name;
    const agent::RunId run{runtime::next_domain(), 1};
    auto executor = std::make_unique<agent::ToolExecutor>(run, std::move(scope), agent::ToolLimits{1, std::chrono::seconds(5)}, nullptr, clock_);
    reply_work_ = executor->submit({{run,1},1}, {"player-reply", name, Json{{"text",order.reply}}.dump()});
    reply_executor_ = std::move(executor);
}

// 所有已验证的玩家原话进入持续会话，技能由模型按委托和实际观察选择。
// 步骤：中断失效回合并结算原回复 → 合并尚待处理的玩家输入 → 绑定工具来源并提交新回合。
void CompanionSession::advance_partner() {
    if (partner_order_) {
        const auto& order = *partner_order_;
        const bool same = !stopping_ && address_ == order.address;
        const bool current = same && inbox_.status().cursor == order.order;
        if (!current) {
            if (conversation_.busy()) conversation_.cancel();
            if (reply_executor_) reply_executor_->close();
        }
        if (reply_executor_) {
            reply_executor_->advance();
            if (reply_work_->exit()->state() == runtime::OperationState::pending) return;
            bool sent = false;
            if (reply_work_->outcome().succeeded) {
                const auto receipt = Json::parse(reply_work_->outcome().output, nullptr, false);
                sent = receipt.is_object() && receipt.value("sent", false);
            }
            if (same) messages_.post(order.address, CompanionSide::game, sent ? CompanionMessageKind::report : CompanionMessageKind::failed,
                messages_.next(CompanionSide::game), order.message, sent ? order.reply : "本轮游戏答复未确认送达");
            // 游戏会先于宿主的下一次收件读回中断旧动作，未送达的旧回合也随新原话交给模型核对。
            if (same && !sent) interrupted_text_ = order.text;
            reply_work_.reset();
            reply_executor_.reset();
            partner_order_.reset();
        } else if (conversation_.busy()) return;
        else if (current) {
            const bool ok = result_ && !result_->error && !result_->answer.empty();
            try { start_partner_reply(ok ? result_->answer : "这次没能完成模型回合，请查看本机模型配置和连接。"); }
            catch (const std::exception&) {
                messages_.post(order.address, CompanionSide::game, CompanionMessageKind::failed,
                    messages_.next(CompanionSide::game), order.message, "游戏回复能力不可用");
                partner_order_.reset();
            }
            return;
        } else {
            if (same) {
                interrupted_text_ = order.text;
                messages_.post(order.address, CompanionSide::game, CompanionMessageKind::failed,
                    messages_.next(CompanionSide::game), order.message, "收到新的玩家消息，旧回合已退出；已发生效果可向游戏重读");
            }
            partner_order_.reset();
        }
    }
    if (stopping_ || !address_ || queued_ || delivering_ || conversation_.busy()) return;
    auto entry = inbox_.take();
    if (!entry) return;
    if (reset_partner_history_) { conversation_.clear(); reset_partner_history_ = false; }
    Json lines = Json::array();
    std::uint64_t seq = 0, message = 0;
    std::string latest;
    do {
        seq = entry->sequence;
        latest = entry->text;
        lines.push_back({{"sequence",seq},{"text",latest}});
        message = messages_.post(*address_, CompanionSide::primary, CompanionMessageKind::delegation,
            messages_.next(CompanionSide::primary), 0, latest).revision;
        entry = inbox_.take();
    } while (entry);
    Json input{{"player",inbox_.status().owner},{"messages",std::move(lines)}};
    if (reader_.state().last) input["lastObservation"] = Json::parse(reader_.state().last->payload);
    if (!interrupted_text_.empty()) { input["interruptedRequest"] = interrupted_text_; interrupted_text_.clear(); }

    partner_order_ = PartnerOrder{*address_,seq,message,std::move(latest),{}};
    result_.reset();
    try {
        agent::RunOptions options;
        options.now = clock_;
        options.tools = catalog_->snapshot(partner_tools_, true).bind_context(source_context(*address_, seq));
        options.version_labels.prompt_template = "game-partner-v1";
        options.system_instructions = "你是大肥鱼，用自己的角色和玩家一起玩。messages 中是绑定玩家的原话；世界观察和工具回执仅描述游戏事实。"
            "理解闲聊、委托和对旧分工的修改。按玩家约定决定是否行动，指定目标失效或需要扩大委托时先问。"
            "使用提供的技能，每次根据实际结果决定下一步。按需观察周围、玩家、背包及上一动作；区分挖掉、入包与交付。"
            "途中有必要才用回复工具，最终文本会自动送到玩家，请用一两句简短中文回复。";
        conversation_.submit(input.dump(), std::move(options));
    } catch (const std::exception&) {
        // 接纳失败时没有模型结果，下一次推进仍向原玩家交付故障提示。
    }
}
}
