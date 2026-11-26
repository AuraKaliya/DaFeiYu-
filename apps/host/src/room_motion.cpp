#include "builtin_room.hpp"
#include <algorithm>

namespace dfy::app {
namespace {
constexpr auto step_time = std::chrono::milliseconds(250);

// 只在既有双向通道上取得另一端，未连接该地点时没有结果。
std::optional<RoomPlace> neighbor(RoomPassage passage, RoomPlace from) noexcept {
    if (passage.from==from) return passage.to;
    if (passage.to==from) return passage.from;
    return {};
}

// 在首间房的开放通道上查找具名节点路径，再依布局距离展开确定性的格位。
// 步骤：核对当前实位 → 遍历可走通道 → 按每段起终点展开逐格位置。
std::optional<std::vector<RoomPoint>> walk_route(const RoomLayout& layout, std::span<const bool> open,
    const RoomSelf& resident, RoomPlace target) {
    const auto place = [&](RoomPlace id) { return std::ranges::find(layout.places,id,&KnownPlace::id); };
    const auto start = place(resident.place);
    if (start==layout.places.end() || start->x!=resident.position.x || start->y!=resident.position.y) return {};
    std::vector<std::vector<RoomPlace>> paths{{resident.place}};
    for (std::size_t i=0;i<paths.size();++i) {
        const auto path = paths[i];
        if (path.back()==target) {
            std::vector<RoomPoint> points;
            for (std::size_t j=1;j<path.size();++j) {
                const auto from=place(path[j-1]), to=place(path[j]);
                const auto passage=std::ranges::find_if(layout.passages,[&](auto link) { return neighbor(link,from->id)==to->id; });
                const int distance=static_cast<int>(passage->distance);
                for (int k=1;k<=distance;++k) points.push_back({
                    static_cast<std::int16_t>(from->x+(to->x-from->x)*k/distance),
                    static_cast<std::int16_t>(from->y+(to->y-from->y)*k/distance)});
            }
            return points;
        }
        for (std::size_t j=0;j<layout.passages.size();++j) {
            const auto next=neighbor(layout.passages[j],path.back());
            if (!open[j] || !next || std::ranges::find(path,*next)!=path.end()) continue;
            auto extended=path;
            extended.push_back(*next);
            paths.push_back(std::move(extended));
        }
    }
    return {};
}
}

// 以同一实例的实际通道更新通行性，在途路线由原动作持有至退出。
bool BuiltinRoom::set_passage_open(RoomBinding room, RoomPlace from, RoomPlace to, bool open) noexcept {
    if (room!=resident_.room || walking()) return false;
    const auto passages=layout().passages;
    for (std::size_t i=0;i<passages.size();++i) if (neighbor(passages[i],from)==to) {
        open_passages_[i]=open;
        return true;
    }
    return false;
}

// 接纳原房间、身体的移动命令，同号重投沿原回执，无路径保留受阻实位。
// 步骤：核对作用域及旧命令 → 采样当前身体与可用路线 → 建立原动作回执 → 安装在途状态。
MoveReply BuiltinRoom::start_move(RoomBinding room, runtime::DomainId body, std::uint64_t command, RoomPlace target,
    agent::ObservationTime now) {
    if (room!=resident_.room || body!=resident_.body || !command) return {MoveAdmission::invalid,{}};
    advance_move(now);
    for (const auto& prior : moves_) if (prior && prior->command==command)
        return {prior->target==target ? MoveAdmission::duplicate : MoveAdmission::conflict,prior};
    if (command<=last_command_) return {MoveAdmission::stale,{}};
    if (walking()) return {MoveAdmission::busy,{}};
    const auto map=layout();
    if (now<scene_time_ || std::ranges::find(map.places,target,&KnownPlace::id)==map.places.end()) return {MoveAdmission::invalid,{}};
    auto path=walk_route(map,open_passages_,resident_,target);
    if (path && now>agent::ObservationTime::max()-step_time*static_cast<std::int64_t>(path->size())) return {MoveAdmission::invalid,{}};
    const auto phase=!path ? MovePhase::blocked : path->empty() ? MovePhase::arrived : MovePhase::walking;
    const auto action=runtime::next_domain();
    if (phase==MovePhase::walking) {
        route_ = std::move(*path);
        walked_ = 0;
        resident_.activity=RoomActivity::walking;
        ++resident_.revision;
        pose_time_=now;
    }
    scene_time_=now;
    last_command_=command;
    move_slot_=(move_slot_+1)%moves_.size();
    auto& record=moves_[move_slot_];
    record=RoomMove{command,action,target,phase,resident_,now,now};
    return {MoveAdmission::accepted,record};
}

// 按原起步时刻逐格记录已发生的位移，与业务线程推进频率无关。
// 步骤：核对在途动作与时间 → 逐格写入实际姿态及经过的具名位置 → 在目标位置结算原回执。
void BuiltinRoom::advance_move(agent::ObservationTime now) noexcept {
    if (!walking() || now<scene_time_) return;
    auto& record=*moves_[move_slot_];
    const auto places=layout().places;
    while (walked_<route_.size()) {
        const auto due=record.started+step_time*static_cast<std::int64_t>(walked_+1);
        if (now<due) break;
        resident_.position=route_[walked_++];
        const auto place=std::ranges::find_if(places,[&](const auto& p) { return p.x==resident_.position.x && p.y==resident_.position.y; });
        if (place!=places.end()) resident_.place=place->id;
        if (walked_==route_.size()) {
            resident_.activity=RoomActivity::resting;
            record.phase=MovePhase::arrived;
        }
        ++resident_.revision;
        record.actual=resident_;
        record.updated=pose_time_=scene_time_=due;
    }
}

// 读取有限保留窗口内的原动作，当前身体或房间不符时不提供历史。
std::optional<RoomMove> BuiltinRoom::movement(RoomBinding room, runtime::DomainId body, runtime::DomainId action) const noexcept {
    if (room!=resident_.room || body!=resident_.body || !action) return {};
    for (const auto& record : moves_) if (record && record->action==action) return record;
    return {};
}
}
