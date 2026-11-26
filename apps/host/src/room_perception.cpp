#include "builtin_room.hpp"
#include <algorithm>

namespace dfy::app {
namespace {
constexpr std::string_view senses_id = "room.senses";
constexpr auto sense_lifetime = std::chrono::seconds(30);

// 固定感知字段；没有看见的灯态与没有取得的方向均保持缺失。
const auto& sense_type() {
    using namespace runtime;
    static const auto type = [] {
        const std::array fields{FieldDescriptor{"kind",ValueKind::string,24},FieldDescriptor{"body",ValueKind::uint64,0},
            FieldDescriptor{"body_revision",ValueKind::uint64,0},FieldDescriptor{"place",ValueKind::string,12,{},true},
            FieldDescriptor{"on",ValueKind::boolean,0,{},true}};
        return std::make_shared<const StructDescriptor>(StructDescriptor::create("room.senses",1,fields,{5,64}));
    }();
    return type;
}
}

// 核对房间、身体、修订及原生采样时间后安装新姿态。
// 步骤：拒绝外部身体 → 识别原样重报 → 按场景水位安装新修订。
bool BuiltinRoom::accept_pose(const RoomSelf& sample, agent::ObservationTime sampled) noexcept {
    if (sample.room!=resident_.room || sample.body!=resident_.body || sample.activity!=RoomActivity::resting) return false;
    if (sample.revision==resident_.revision) return sampled==pose_time_ && sample.place==resident_.place &&
        sample.position.x==resident_.position.x && sample.position.y==resident_.position.y;
    if (sample.revision<resident_.revision || sampled<scene_time_ ||
        std::ranges::none_of(layout().places,[&](const auto& place) { return place.id==sample.place; })) return false;
    resident_ = sample;
    pose_time_ = scene_time_ = sampled;
    return true;
}

// 构造不可变的角色观察，修订只随该角色取得的新采样递增。
// 步骤：写入采样类型与身体 → 仅追加确实可知的字段 → 签发并推进观察修订。
agent::ObservationSnapshot BuiltinRoom::sense_sample(std::string_view kind, bool from_desk, std::optional<bool> on,
    agent::ObservationTime sampled) {
    using namespace runtime;
    std::vector<NamedValue> fields{{"kind",Value::from_string(kind,24)},{"body",Value::from_uint64(resident_.body)},
        {"body_revision",Value::from_uint64(resident_.revision)}};
    if (from_desk) fields.push_back({"place",Value::from_string("desk",12)});
    if (on) fields.push_back({"on",Value::from_bool(*on)});
    auto payload = std::make_shared<const StructValue>(StructValue::create(sense_type(),fields,128));
    auto result = source_.capture({std::string(senses_id),resident_.room.generation},sense_revision_+1,
        sampled,sampled+sense_lifetime,std::move(payload));
    ++sense_revision_;
    return result;
}

// 环境接单时冻结感官范围，迟到送达或重报不会按新位置扩写旧事件。
// 步骤：核验环境实例和修订 → 取发生时身体筛选 → 更新私有灯态并缓存原感知。
std::optional<agent::ObservationSnapshot> BuiltinRoom::capture_switch(LampSwitch change, std::uint64_t revision,
    agent::ObservationTime sampled) {
    if (change.room!=resident_.room || !revision) return {};
    if (revision==switch_revision_)
        return lamp_on_==change.on && sampled==switch_time_ ? last_switch_ : std::nullopt;
    if (revision<switch_revision_ || sampled<scene_time_ || sampled>agent::ObservationTime::max()-sense_lifetime) return {};

    std::optional<agent::ObservationSnapshot> result;
    if (lamp_on_!=change.on) {
        if (const auto sense=perceive_switch(resident_,change)) {
            if (const auto* seen=std::get_if<SeenLampChange>(&*sense)) result=sense_sample("seen_switch",true,seen->on,sampled);
            else result=sense_sample("heard_switch",true,{},sampled);
        }
    }
    lamp_on_ = change.on;
    switch_revision_ = revision;
    scene_time_ = switch_time_ = sampled;
    return last_switch_ = result;
}

// 原生设备只有向本房间指定角色报告读数的权限，重新签发同号不能改变原值。
// 步骤：核对收件身体与原设备水位 → 制作仅含报告读数的载荷 → 以独立设备来源签发。
std::optional<agent::ObservationSnapshot> BuiltinRoom::capture_lamp_report(RoomBinding room, runtime::DomainId body,
    bool reported_on, std::uint64_t revision, agent::ObservationTime sampled) {
    using namespace runtime;
    if (room!=resident_.room || body!=resident_.body || !revision || sampled>agent::ObservationTime::max()-sense_lifetime) return {};
    if (last_report_) {
        const auto& previous = last_report_->metadata();
        if (revision==previous.revision)
            return sampled==previous.sampled && last_report_->payload()->find("on")->as_bool()==reported_on ? last_report_ : std::nullopt;
        if (revision<previous.revision || sampled<previous.sampled) return {};
    }
    static const auto type = [] {
        const std::array fields{FieldDescriptor{"body",ValueKind::uint64,0},FieldDescriptor{"on",ValueKind::boolean,0}};
        return std::make_shared<const StructDescriptor>(StructDescriptor::create("room.lamp_monitor",1,fields,{2,64}));
    }();
    const std::array fields{NamedValue{"body",Value::from_uint64(body)},NamedValue{"on",Value::from_bool(reported_on)}};
    auto payload = std::make_shared<const StructValue>(StructValue::create(type,fields,64));
    return last_report_ = monitor_.capture({"room.lamp_monitor",room.generation},revision,sampled,sampled+sense_lifetime,std::move(payload));
}

// 仅已接纳的本角色事件占用被动记录游标，个人感知与设备报告各有原来源水位。
// 步骤：接纳原快照 → 核对实际结果 → 在固定环形记录中保存原证据。
agent::ObservationReceipt BuiltinRoom::receive(const agent::ObservationSnapshot& sample, agent::ObservationTime now) {
    auto& inbox = sample.metadata().source==monitor_.id() ? monitor_inbox_ : inbox_;
    const auto receipt = inbox.accept(sample,now);
    if (receipt.status==agent::ObservationAdmission::inserted || receipt.status==agent::ObservationAdmission::replaced) {
        history_[received_%history_.size()] = inbox.latest(sample.metadata().source,sample.metadata().object.id);
        ++received_;
    }
    return receipt;
}

// 主动查询只给出查询时能看见的当前灯态，听觉事件没有可重放的现场声音。
// 步骤：核对当前角色与时间 → 按同一视距和屏风判定取样 → 依原水位建立完整基线。
std::shared_ptr<const agent::ObservationRecord> BuiltinRoom::observe_current(RoomBinding room, runtime::DomainId body,
    agent::ObservationTime now) {
    if (room!=resident_.room || body!=resident_.body || now<scene_time_ || now>agent::ObservationTime::max()-sense_lifetime) return {};
    const auto ticket = inbox_.refresh_ticket(source_.id(),senses_id);
    const auto sense = perceive_switch(resident_,{room,lamp_on_.value_or(false)});
    const bool visible = sense && std::holds_alternative<SeenLampChange>(*sense);
    const auto reading = sense_sample(visible && lamp_on_ ? "lamp_read" : "unconfirmed",visible,visible ? lamp_on_ : std::nullopt,now);
    const auto receipt = ticket ? inbox_.refresh(*ticket,reading,now) : inbox_.accept_initial_baseline(reading,now);
    if (receipt.status!=agent::ObservationAdmission::inserted && receipt.status!=agent::ObservationAdmission::replaced) return {};
    scene_time_ = now;
    return inbox_.latest(source_.id(),senses_id);
}

// 导出本角色已收事件，历史游标过旧时给出当前仍保留的窗口及缺口。
std::optional<RoomPerceptPage> BuiltinRoom::percepts_since(RoomBinding room, runtime::DomainId body, std::uint64_t cursor) const {
    if (room!=resident_.room || body!=resident_.body || cursor>received_) return {};
    const auto oldest = received_>history_.size() ? received_-history_.size() : 0;
    RoomPerceptPage page{received_,cursor<oldest,{}};
    for (auto position=std::max(cursor,oldest); position<received_; ++position)
        page.records.push_back(history_[position%history_.size()]);
    return page;
}
}
