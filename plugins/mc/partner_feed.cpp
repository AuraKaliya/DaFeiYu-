#include "partner_feed.hpp"
#include <algorithm>

namespace dfy::mc {
using Json = nlohmann::json;

// 归还旧玩家事件，保留原请求编号不会重用的约束。
void PartnerFeed::reset_binding() noexcept {
    owner_.clear();
    cursor_ = departed_ = departure_ = 0;
    scene_revision_ = 0;
    ready_ = more_ = gap_ = false;
    pending_.clear();
    anchored_messages_.clear();
    control_ = nullptr;
}

// 停止读口并撤销原世界状态。
void PartnerFeed::clear() noexcept {
    pipe_.reset();
    generation_.clear();
    observation_.clear();
    next_ = world_due_ = {};
    reset_binding();
}

// 仅在子会话接纳后丢弃待交付窗口。
void PartnerFeed::consumed() { pending_.clear(); gap_ = false; }

// 在旧工作结算后裁剪旧在场周期，后续事实仍按水位进入上下文。
// 步骤：从待交付事件和原话锚点裁剪 → 归还已处理的离场水位。
void PartnerFeed::settle_departure() {
    pending_.erase(pending_.begin(),std::ranges::find_if(pending_,[&](const auto& event) {
        return event.at("sequence") > departure_;
    }));
    while (!anchored_messages_.empty() && anchored_messages_.front().at("sequence") <= departure_)
        anchored_messages_.pop_front();
    departure_ = 0;
}

// 只从当前世界已收的原消息中查找对应现场。
const Json* PartnerFeed::message_scene(std::uint64_t sequence) const noexcept {
    for (const auto& event : anchored_messages_)
        if (event.at("sequence") == sequence) return &event.at("sceneAtMessage");
    return nullptr;
}

// 核对原世界阶段并更新本轮可用观察。
void PartnerFeed::accept_world(std::string_view wire) {
    auto world = Json::parse(world_reply(wire,id_));
    auto generation = world.is_null() ? std::string{} : world.at("generation").get<std::string>();
    if (generation != generation_) reset_binding();
    generation_ = std::move(generation);
    observation_ = std::move(world);
}

// 只交付经过协议校验的在场事实与原话，持续推进原玩家水位。
// 步骤：核对游戏和玩家 → 按基线或缺口重建窗口 → 追加连续事实及更新离场状态。
void PartnerFeed::accept_inbox(std::string_view wire) {
    const auto batch = Json::parse(inbox_reply(wire,id_,arguments_));
    if (batch.is_null()) { clear(); return; }
    auto owner = batch.at("owner").get<std::string>();
    if (!owner_.empty() && owner != owner_) { clear(); return; }
    const auto latest = batch.at("latest").get<std::uint64_t>();
    const auto oldest = batch.at("oldest").get<std::uint64_t>();
    while (!anchored_messages_.empty() && anchored_messages_.front().at("sequence").get<std::uint64_t>() < oldest)
        anchored_messages_.pop_front();
    const auto& facts = batch.at("messages");
    const auto presence = batch.find("presence");
    if (presence != batch.end()) departed_ = presence->at("type") == "player.left" ? presence->at("sequence").get<std::uint64_t>() : 0;
    auto latest_departure = batch.value("lastDeparture",departed_);
    for (const auto& fact : facts) if (fact.value("type","") == "player.left")
        latest_departure = std::max(latest_departure,fact.at("sequence").get<std::uint64_t>());
    if ((ready_ || departed_) && latest_departure > cursor_) departure_ = std::max(departure_,latest_departure);
    if (!ready_ || batch.at("gap") == true || pending_.size()+facts.size() > 32) {
        gap_ |= ready_;
        pending_.clear();
        if (presence != batch.end() && presence->at("sequence") > cursor_) pending_.push_back(*presence);
        cursor_ = latest;
        more_ = false;
    } else {
        for (const auto& fact : facts) {
            Json event{{"sequence",fact.at("sequence")},{"type",fact.value("type","player.message")}};
            if (fact.contains("text")) event["text"] = fact.at("text");
            if (fact.contains("sceneAtMessage")) {
                event["sceneAtMessage"] = fact.at("sceneAtMessage");
                if (anchored_messages_.size() >= 32) anchored_messages_.pop_front();
                anchored_messages_.push_back(event);
            }
            pending_.push_back(std::move(event));
            cursor_ = fact.at("sequence").get<std::uint64_t>();
        }
        more_ = batch.at("more").get<bool>();
    }
    if (batch.contains("control")) control_ = batch.at("control");
    if (batch.contains("sceneRevision")) {
        const auto revision = batch.at("sceneRevision").get<std::uint64_t>();
        if (revision < scene_revision_) throw std::invalid_argument("MC feed: scene revision regressed");
        scene_revision_ = revision;
    }
    owner_ = std::move(owner);
    ready_ = true;
}

// 按原绑定周期获取世界观察和玩家事实，所有 I/O 步骤均让出宿主。
// 步骤：选择只读查询 → 非等待推进管道 → 回收本次连接并交付验证结果 → 失联时撤销绑定。
void PartnerFeed::advance(const BridgeConfig& config, std::uint64_t owner) {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if (now < next_) return;
    try {
        if (!pipe_) {
            if (sequence_ == UINT64_MAX) throw std::overflow_error("MC feed: request sequence exhausted");
            id_ = "feed."+std::to_string(owner)+"."+std::to_string(++sequence_);
            reading_world_ = generation_.empty() || now >= world_due_;
            if (!reading_world_) arguments_ = Json{{"generation",generation_},{"after",cursor_}}.dump();
            auto request = reading_world_ ? world_request(id_) : inbox_request(id_,config.key,arguments_);
            pipe_ = std::make_unique<PipeExchange>(config.channel,std::move(request));
        }
        const auto reply = pipe_->poll();
        if (!reply) return;
        pipe_.reset();
        if (reading_world_) {
            accept_world(*reply);
            world_due_ = now+seconds(1);
            next_ = generation_.empty() ? world_due_ : now;
        } else {
            accept_inbox(*reply);
            next_ = more_ ? now : now+milliseconds(100);
        }
    } catch (const std::exception&) {
        clear();
        next_ = now+milliseconds(250);
    }
}
}
