#include "partner_feed.hpp"
#include <algorithm>

namespace dfy::mc {
using Json = nlohmann::json;

// 归还旧玩家事件，保留原请求编号不会重用的约束。
void PartnerFeed::reset_binding() noexcept {
    if (ready_) ++binding_revision_;
    owner_.clear();
    mode_.clear();
    execution_.clear();
    cursor_ = departed_ = departure_ = 0;
    scene_revision_ = 0;
    air_cursor_ = 0;
    air_requested_ = air_legacy_ = air_confirmed_ = false;
    air_notices_.clear();
    recover_until_ = {};
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
// 步骤：校验来源和现场修订 → 组装完整收件窗口 → 一并提交水位、消息和执行状态。
void PartnerFeed::accept_inbox(std::string_view wire) {
    const auto batch = Json::parse(inbox_reply(wire,id_,arguments_));
    if (batch.is_null()) { clear(); return; }
    auto owner = batch.at("owner").get<std::string>();
    if (!owner_.empty() && owner != owner_) { clear(); return; }
    const auto scene_revision = batch.value("sceneRevision",scene_revision_);
    if (scene_revision < scene_revision_) throw std::invalid_argument("MC feed: scene revision regressed");
    const auto latest = batch.at("latest").get<std::uint64_t>();
    const auto oldest = batch.at("oldest").get<std::uint64_t>();
    const auto& facts = batch.at("messages");
    const auto presence = batch.find("presence");
    const auto execution = batch.find("execution");
    auto mode = execution == batch.end() ? std::string{} : execution->at("mode").get<std::string>();
    auto state = execution == batch.end() ? std::string("unreported") : execution->at("state").get<std::string>();
    auto control = batch.value("control",control_);
    const bool permitted = mode == "partner" && state == "ready";
    const bool acquired = ready_ && permitted && !execution_bound();

    const auto departed = presence == batch.end() ? departed_ : presence->at("type") == "player.left" ?
        presence->at("sequence").get<std::uint64_t>() : 0;
    auto latest_departure = batch.value("lastDeparture",departed);
    for (const auto& fact : facts) if (fact.value("type","") == "player.left")
        latest_departure = std::max(latest_departure,fact.at("sequence").get<std::uint64_t>());
    const auto reset_window = !ready_ || acquired || batch.at("gap") == true || pending_.size()+facts.size() > 32;
    auto pending = reset_window ? Json::array() : pending_;
    auto anchored = anchored_messages_;
    auto cursor = cursor_;
    while (!anchored.empty() && anchored.front().at("sequence").get<std::uint64_t>() < oldest) anchored.pop_front();
    if (reset_window) {
        if (presence != batch.end() && presence->at("sequence") > cursor) pending.push_back(*presence);
        cursor = latest;
    } else {
        for (const auto& fact : facts) {
            Json event{{"sequence",fact.at("sequence")},{"type",fact.value("type","player.message")}};
            if (fact.contains("text")) event["text"] = fact.at("text");
            if (fact.contains("sceneAtMessage")) {
                event["sceneAtMessage"] = fact.at("sceneAtMessage");
                if (anchored.size() >= 32) anchored.pop_front();
                anchored.push_back(event);
            }
            pending.push_back(std::move(event));
            cursor = fact.at("sequence").get<std::uint64_t>();
        }
    }

    if ((ready_ || departed) && latest_departure > cursor_) departure_ = std::max(departure_,latest_departure);
    // 新持权者从接管水位接收，等候期间收集的旧原话不进入它的子 Agent。
    if (acquired) { gap_ = false; departure_ = 0; anchored.clear(); }
    else gap_ |= ready_ && reset_window;
    if (!permitted || departed || departure_) air_notices_.clear();
    if (const auto notice = batch.find("airNotice"); notice != batch.end()) {
        const auto sequence = notice->at("sequence").get<std::uint64_t>();
        if (!departed && !departure_ && notice->at("actor") == observation_.at("actor") && notice->at("dimension") == observation_.at("dimension")) {
            auto fact = *notice;
            if (air_cursor_ && sequence > air_cursor_+1) fact["missedBefore"] = sequence-air_cursor_-1;
            if (air_notices_.size() == 4) {
                const auto missing = air_notices_.front().value("missedBefore",std::uint64_t{})+1;
                air_notices_.erase(air_notices_.begin());
                air_notices_.front()["missedBefore"] = air_notices_.front().value("missedBefore",std::uint64_t{})+missing;
            }
            air_notices_.push_back(std::move(fact));
        }
        air_cursor_ = sequence;
    }
    if (execution_bound() && !permitted) ++binding_revision_;
    more_ = !reset_window && batch.at("more").get<bool>();
    cursor_ = cursor;
    departed_ = departed;
    scene_revision_ = scene_revision;
    pending_ = std::move(pending);
    anchored_messages_ = std::move(anchored);
    control_ = std::move(control);
    mode_ = std::move(mode);
    execution_ = std::move(state);
    owner_ = std::move(owner);
    ready_ = true;
    if (air_requested_) air_confirmed_ = true;
    recover_until_ = {};
}

// 按原绑定周期获取世界观察和玩家事实，所有 I/O 步骤均让出宿主。
// 步骤：检查恢复期限 → 推进只读查询 → 核验原绑定或续读 → 仅在确认失联时放弃绑定。
void PartnerFeed::advance(const BridgeConfig& config, std::uint64_t owner) {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if (recovering() && now >= recover_until_) { clear(); return; }
    if (now < next_) return;
    try {
        if (!pipe_) {
            if (sequence_ == UINT64_MAX) throw std::overflow_error("MC feed: request sequence exhausted");
            id_ = "feed."+std::to_string(owner)+"."+std::to_string(++sequence_);
            reading_world_ = generation_.empty() || now >= world_due_;
            if (!reading_world_) {
                Json args{{"generation",generation_},{"after",cursor_}};
                air_requested_ = ready_ && !air_legacy_;
                if (air_requested_) args["airAfter"] = air_cursor_;
                arguments_ = args.dump();
            }
            auto request = reading_world_ ? world_request(id_) : inbox_request(id_,config.key,arguments_,config.controller);
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
        // 旧桥接只支持基础监听。沿原聊天游标重读，遥测扩展失败不会吞去原话。
        if (!reading_world_ && air_requested_ && !air_confirmed_) {
            air_legacy_ = true;
            air_requested_ = false;
            pipe_.reset();
            next_ = now;
            return;
        }
        if (ready_) {
            pipe_.reset();
            if (!recovering()) recover_until_ = now+seconds(3);
            world_due_ = {};
        } else clear();
        next_ = now+milliseconds(250);
    }
}
}
