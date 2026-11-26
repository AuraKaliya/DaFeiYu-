#include "builtin_room.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace dfy;
using namespace dfy::app;
using namespace std::chrono_literals;
using A = agent::ObservationAdmission;

// 在固定的原生场景断言失败时保留首个丢失的角色边界。
void check(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

// 经过真实房间的身体采样入口更新当前位置，同时保留当前身体及房间代次。
void position(BuiltinRoom& room, RoomSelf& resident, RoomPlace place, RoomPoint point, agent::ObservationTime time) {
    ++resident.revision;
    resident.place = place;
    resident.position = point;
    check(room.accept_pose(resident,time) && room.accept_pose(resident,time),"native pose or duplicate lost");
}

// 核对事件按原姿态采样、重报去重、主动读取不泄露灯态、现场重采样与历史隔离。
// 步骤：屏风后冻结听觉 → 桌前送达和主动读取 → 远处忽略变化 → 断档后仅按现地重采样。
void verify_room() {
    BuiltinRoom room, other;
    const auto binding=room.layout().room, foreign=other.layout().room;
    auto resident=room.self(binding).value();
    const auto t=agent::ObservationTime{};
    const auto observe=[&](auto time) { return room.observe_current(binding,resident.body,time); };
    const auto initial=observe(t);
    check(initial && !initial->payload().find("on") && initial->payload().find("kind")->as_string()=="unconfirmed",
        "a lamp value was invented before the first native sample");
    position(room,resident,RoomPlace::behind_screen,{4,3},t+1s);
    const auto heard=room.capture_switch({binding,true},1,t+2s).value();
    auto conflict=resident;
    conflict.position={4,0};
    check(!room.accept_pose(conflict,t+1s) && !other.accept_pose(resident,t+1s) &&
        room.receive(heard,t+1s).status==A::future,"conflicting pose or future signal crossed the current boundary");
    check(heard.payload()->find("kind")->as_string()=="heard_switch" && !heard.payload()->find("on") &&
        heard.payload()->find("place")->as_string()=="desk" && !heard.payload()->find("operator") &&
        heard.metadata().provider_instance==binding.instance && heard.metadata().sampled==t+2s,"heard private light state");
    position(room,resident,RoomPlace::desk,{4,0},t+3s);
    const auto repeated=room.capture_switch({binding,true},1,t+2s).value();
    check(repeated.payload()==heard.payload() && !room.capture_switch({binding,false},1,t+2s) &&
        room.receive(heard,t+3s).status==A::replaced && room.receive(repeated,t+3s).status==A::duplicate &&
        other.receive(heard,t+3s).status==A::not_allowed,"late or foreign arrival changed issuer evidence");
    const auto nearby=observe(t+4s);
    check(nearby && nearby->payload().find("on")->as_bool() && nearby->metadata().sampled==t+4s &&
        nearby->payload().find("body_revision")->as_uint64()==resident.revision,"nearby active sample lost current state");
    position(room,resident,RoomPlace::window,{-2,0},t+5s);
    check(!room.capture_switch({binding,false},2,t+6s) && !room.capture_switch({binding,false},3,t+6s) &&
        !room.capture_switch({foreign,true},4,t+6s) &&
        !observe(t+5s) && !room.observe_current(binding,other.self(foreign)->body,t+6s),"hidden or wrong-scope sample leaked");
    const auto hidden=observe(t+7s);
    check(hidden && !hidden->payload().find("on") && !hidden->payload().find("place"),"read revealed a hidden current object");
    position(room,resident,RoomPlace::desk,{4,0},t+8s);
    check(!observe(t+8s)->payload().find("on")->as_bool(),"walking closer did not obtain the new local light state");
    const auto missed=room.capture_switch({binding,true},4,t+9s).value();
    const auto seen=room.capture_switch({binding,false},5,t+10s).value();
    const auto receipt=room.receive(seen,t+10s);
    check(receipt.status==A::replaced && receipt.revision_gap && room.receive(missed,t+11s).status==A::stale_revision &&
        room.receive(missed,t+40s).status==A::expired,"missed, late or expired sense was accepted as fresh");
    position(room,resident,RoomPlace::behind_screen,{4,3},t+11s);
    const auto refreshed=observe(t+12s);
    check(refreshed && !refreshed->revision_gap() && !refreshed->payload().find("on") &&
        !refreshed->payload().find("place"),"gap recovery disclosed a hidden lamp or replayed a sound");
    const auto history=room.percepts_since(binding,resident.body,0).value();
    check(history.cursor==2 && !history.history_gap && history.records.size()==2 &&
        history.records[0]->payload().find("kind")->as_string()=="heard_switch" && !history.records[0]->payload().find("on") &&
        history.records[1]->payload().find("kind")->as_string()=="seen_switch" && !history.records[1]->payload().find("on")->as_bool() &&
        !room.percepts_since(foreign,resident.body,0) && !room.percepts_since(binding,resident.body,3),"passive history crossed its recorded scope");
}

// 检验首个完整读可越过尚未送达的感知，过期和旧游标不会恢复已淘汰的记录。
// 步骤：先签发未投递事件 → 完整读取当前状态 → 填满环形记录 → 从边界游标核对窗口。
void verify_history() {
    BuiltinRoom room;
    const auto binding=room.layout().room;
    const auto resident=room.self(binding).value();
    const auto t=agent::ObservationTime{};
    const auto unreceived=room.capture_switch({binding,false},1,t).value();
    const auto baseline=room.observe_current(binding,resident.body,t+1s);
    check(baseline && !baseline->revision_gap() && !baseline->payload().find("on")->as_bool() &&
        room.receive(unreceived,t+1s).status==A::stale_revision,"first complete reading inherited a delivery gap");
    for (unsigned i=1;i<=34;++i) {
        const auto time=t+std::chrono::seconds(i);
        const auto sample=room.capture_switch({binding,i%2!=0},i+1,time).value();
        check(room.receive(sample,time).status==A::replaced,"sample was not retained");
    }
    const auto old=room.percepts_since(binding,resident.body,0).value(), recent=room.percepts_since(binding,resident.body,32).value();
    check(old.history_gap && old.cursor==34 && old.records.size()==32 && old.records.front()->metadata().sampled==t+3s &&
        !recent.history_gap && recent.records.size()==2 && room.percepts_since(binding,resident.body,34)->records.empty(),
        "bounded history silently lost a cursor gap");
}
}

// 以固定原生采样时间验证房间感知和实际当前读数。
int main() {
    try { verify_room(); verify_history(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
