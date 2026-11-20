#include "companion.hpp"
#include <dafeiyu/companion_abi_v2.h>
#include <nlohmann/json.hpp>

namespace dfy::app {
namespace {
// 在受保护回调内复制插件借出的有界身份。
std::string identity(dfy_v2_string_view value) {
    if (value.length > 128 || (value.length && !value.data)) throw std::invalid_argument("companion: invalid partner identity");
    return value.length ? std::string(value.data,value.length) : std::string{};
}
}

// 宿主只提交启动和停止意图，循环及其子会话由原 DLL 持有。
void CompanionSession::switch_partner(plugins::PluginInstance& instance, bool listen) {
    const auto capability = instance.query<dfy_v2_companion_partner_api>(DFY_V2_COMPANION_PARTNER_INTERFACE,DFY_V2_COMPANION_PARTNER_VERSION);
    if (!capability) throw std::invalid_argument("companion: plugin-owned partner unavailable");
    instance.invoke(*capability,[listen](const auto& api, auto* plugin) {
        const auto command = listen ? api.listen : api.stop;
        if (!command || command(plugin,nullptr)) throw std::runtime_error("companion: partner command rejected");
    });
}

// 读取插件自己的状态及信箱事实，宿主只维护可供界面查询的值快照。
// 步骤：核对原实例 → 复制角色状态及绑定 → 投影已收到的事实 → 在正式停止后释放原地址。
void CompanionSession::advance_partner() {
    auto& projection = *partner_;
    if (!bound()) {
        projection.closed = true;
        projection.input.ready = false;
        address_.reset();
        messages_.bind(std::nullopt);
        return;
    }
    const auto instance = instance_.lock();
    const auto capability = instance->query<dfy_v2_companion_partner_api>(DFY_V2_COMPANION_PARTNER_INTERFACE,DFY_V2_COMPANION_PARTNER_VERSION);
    if (!capability) throw std::logic_error("companion: original partner capability unavailable");
    const auto [phase,world] = instance->invoke(*capability,[&](const auto& api, auto* plugin) {
        dfy_v2_partner_status value{};
        if (!api.status || api.status(plugin,&value,nullptr) || value.ready > 1 ||
            value.phase < DFY_V2_PARTNER_CLOSED || value.phase > DFY_V2_PARTNER_STOPPING)
            throw std::runtime_error("companion: invalid partner status");
        projection.input.ready = value.ready != 0;
        projection.input.cursor = value.cursor;
        projection.input.departed = value.departed;
        projection.input.owner = identity(value.player);
        if (value.settled || value.phase == DFY_V2_PARTNER_LISTENING) projection.settled = value.settled;
        return std::pair{value.phase,identity(value.world)};
    });
    if (!world.empty() && (!address_ || address_->world != world)) {
        CompanionAddress current{id(),offer_.generation,world,runtime::next_domain()};
        messages_.bind(current);
        address_ = std::move(current);
        projection.revisions.fill({});
        projection.input.gap = false;
    }
    for (std::size_t count = 0; count < 16; ++count) {
        auto item = mailboxes_.network->read(mailboxes_.identity,projection.inbox);
        if (!item.event) break;
        try { accept_partner(*item.event.get().payload); }
        catch (const nlohmann::json::exception&) { projection.input.gap = true; }
    }
    projection.closed = phase == DFY_V2_PARTNER_CLOSED;
    if (phase != DFY_V2_PARTNER_LISTENING && !stopping_) close();
    if (projection.closed || (world.empty() && phase == DFY_V2_PARTNER_LISTENING)) {
        address_.reset();
        messages_.bind(std::nullopt);
    }
}

// 将原角色已观察或真正完成的事实投影为原计划板消息。
// 步骤：核对分类与游戏代次 → 关联原事件修订 → 复制真实发言、失败或控制回执。
void CompanionSession::accept_partner(const runtime::StructValue& payload) {
    const auto descriptor = payload.descriptor();
    if (descriptor->version() != 1) return;
    const auto type = descriptor->name();
    const auto* text = payload.find("text");
    if (!text || text->kind() != runtime::ValueKind::string) return;
    if (type.starts_with("mc.confirmation.")) { accept_confirmation(type,text->as_string()); return; }
    const auto data = nlohmann::json::parse(text->as_string(),nullptr,false);
    if (!data.is_object() || !data.contains("generation")) return;
    if (type == "mc.partner.control") {
        const auto phase = data.at("phase");
        control_ = ControlReceipt{data.at("generation"),data.at("order"),
            phase == "released" ? ControlPhase::released : ControlPhase::unconfirmed};
        partner_->input.control = control_;
        return;
    }
    if (!address_ || data.at("generation") != address_->world) return;
    if (type == "mc.partner.event") {
        const auto& fact = data.at("event");
        const auto order = fact.at("sequence").get<std::uint64_t>();
        const auto kind = fact.at("type").get<std::string>();
        const bool speech = kind == "player.message";
        const auto message = messages_.post(*address_,CompanionSide::primary,
            speech ? CompanionMessageKind::utterance : CompanionMessageKind::event,messages_.next(CompanionSide::primary),
            0,speech ? fact.at("text").get<std::string>() : kind);
        partner_->revisions[order%partner_->revisions.size()] = {order,message.revision};
        return;
    }
    if (type != "mc.partner.action" && type != "mc.partner.turn") return;
    const auto order = data.at("order").get<std::uint64_t>();
    const auto [known,revision] = partner_->revisions[order%partner_->revisions.size()];
    if (known != order || !revision) return;
    if (type == "mc.partner.turn" && data.at("failed") == true)
        messages_.post(*address_,CompanionSide::game,CompanionMessageKind::failed,messages_.next(CompanionSide::game),revision,"游戏决策回合未完成");
    if (type == "mc.partner.action" && data.at("skill") == "reply" && data.contains("result")) {
        const auto& result = data.at("result");
        if (result.value("sent",false) && !result.value("reused",false))
            messages_.post(*address_,CompanionSide::game,CompanionMessageKind::report,messages_.next(CompanionSide::game),revision,result.at("text"));
    }
}

// 从原插件的已验证地址取得业务请求与最终裁定，迟到回执不覆盖新请求。
// 步骤：匹配当前世界和身份 → 登记原候选或匹配回执 → 保存唯一结果。
void CompanionSession::accept_confirmation(std::string_view type, std::string_view bytes) {
    const auto data = nlohmann::json::parse(bytes,nullptr,false);
    if (!data.is_object() || !data.at("id").is_number_unsigned() || !data.at("revision").is_number_unsigned() ||
        data.at("revision") != 1 || !data.at("generation").is_string() || !data.at("player").is_string()) return;
    const auto id = data.at("id").get<std::uint64_t>();
    const auto& world = data.at("generation").get_ref<const std::string&>();
    const auto& player = data.at("player").get_ref<const std::string&>();
    if (type == "mc.confirmation.request") {
        if (!id || stopping_ || !address_ || world != address_->world || player != partner_->input.owner) return;
        if (confirmation_ && confirmation_->id == id) return;
        confirmation_ = CompanionConfirmation{id,1,data.at("order").get<std::uint64_t>(),world,player,
            data.at("question").get<std::string>(),data.at("arguments").dump()};
    } else if (confirmation_ && confirmation_->id == id && confirmation_->world == world &&
        confirmation_->player == player && type == "mc.confirmation.presentation") {
        confirmation_->presentation = data.at("status");
    } else if (confirmation_ && confirmation_->id == id && confirmation_->world == world &&
        confirmation_->player == player && type == "mc.confirmation.decision") {
        if (!confirmation_->decision.empty()) return;
        confirmation_->decision = data.at("status");
        confirmation_->source = data.at("source");
    }
}

// 路由原问题，模型选择通道后由宿主提交对应动作。
// 步骤：核对原请求 → 固定首次渠道 → 提交游戏投递或产生本体可见问题。
std::string CompanionSession::route_confirmation(std::uint64_t request, bool game) {
    if (!partner_ || !confirmation_ || confirmation_->id != request || !confirmation_->decision.empty() ||
        !address_ || confirmation_->world != address_->world || stopping_ || !bound())
        throw std::invalid_argument("companion: confirmation is no longer current");
    auto& item = *confirmation_;
    if (!item.channel.empty()) throw std::logic_error("companion: confirmation channel already selected");
    if (game) {
        const auto message = nlohmann::json{{"id",item.id},{"revision",item.revision},{"generation",item.world},{"player",item.player}}.dump();
        mailboxes_.network->deliver_next(mailboxes_.identity,partner_->outgoing,
            events::mailbox_text("mc.confirmation.present",1,message,mailboxes_.network->payload_limit()));
        item.channel = "game";
        return "Game presentation was queued. Wait for the separate player decision and delivery receipt.";
    }
    item.channel = "panel";
    return item.question+"  /companion confirm "+std::to_string(item.id)+" "+std::to_string(item.revision)+" approve|deny";
}

// 转发本体用户的原决定，输送到信箱仅表示接纳。
// 步骤：核对原问题和修订 → 按当前会话发送决定 → 保留已送状态供重复查询。
void CompanionSession::reply_confirmation(std::uint64_t id, std::uint64_t revision, bool approve) {
    if (!partner_ || !confirmation_ || confirmation_->id != id || confirmation_->revision != revision)
        throw std::invalid_argument("companion: confirmation or revision unavailable");
    auto& item = *confirmation_;
    if (!item.decision.empty() || item.answered) return;
    if (stopping_ || !bound() || !address_ || item.world != address_->world) throw std::invalid_argument("companion: confirmation binding expired");
    const auto decision = nlohmann::json{{"id",id},{"revision",revision},{"generation",item.world},{"player",item.player},{"approve",approve}}.dump();
    mailboxes_.network->deliver_next(mailboxes_.identity,partner_->outgoing,
        events::mailbox_text("mc.confirmation.response",1,decision,mailboxes_.network->payload_limit()));
    item.answered = true;
}
}
