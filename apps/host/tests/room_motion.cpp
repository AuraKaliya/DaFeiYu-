#include "application.hpp"
#include <iostream>

namespace {
using namespace dfy;
using namespace dfy::app;
using namespace std::chrono_literals;

// 报告指定节奏、原动作或位置断言的首个差异。
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// 从正式应用业务线程推进一次桌边到窗边的移动，并在路上按当时位置采样。
// 步骤：接纳原动作与重投 → 按指定节奏读在途实位 → 到达后核对原动作、时刻和感知。
std::uint64_t drive(const std::filesystem::path& data, std::chrono::milliseconds cadence) {
    agent::ObservationTime now{};
    ApplicationRunConfig config;
    config.tool_clock=[&] { return now; };
    unsigned models=0;
    Application application({},data/"empty",data,model::Backend{[&](const auto&,const auto&) {
        ++models; return model::Response{{model::Role::assistant,"固定"}};
    }},config);
    auto& room=application.room();
    const auto binding=room.layout().room;
    const auto resident=room.self(binding).value();
    const auto started=room.start_move(binding,resident.body,1,RoomPlace::window,now);
    check(started.admission==MoveAdmission::accepted && started.record->phase==MovePhase::walking && room.walking(),"move not admitted");
    const auto id=started.record->action;
    check(room.start_move(binding,resident.body,1,RoomPlace::window,now).record->action==id &&
        room.start_move(binding,resident.body,1,RoomPlace::behind_screen,now).admission==MoveAdmission::conflict &&
        room.start_move(binding,resident.body,2,RoomPlace::desk,now).admission==MoveAdmission::busy,"replayed or replaced original command");
    for (auto elapsed=cadence;elapsed<=500ms;elapsed+=cadence) {
        now=agent::ObservationTime{}+elapsed;
        application.advance_plugins();
    }
    const auto middle=room.movement(binding,resident.body,id).value();
    check(middle.phase==MovePhase::walking && middle.actual.position.x==2 && middle.actual.place==RoomPlace::desk,
        "walking pose missing at half distance");
    now=agent::ObservationTime{}+1s;
    const auto sound=room.capture_switch({binding,true},1,now).value();
    check(sound.payload()->find("kind")->as_string()=="heard_switch" && !sound.payload()->find("on"),"event sampled pre-arrival pose");
    application.advance_plugins();
    const auto end=room.movement(binding,resident.body,id).value();
    check(end.phase==MovePhase::arrived && end.actual.place==RoomPlace::window && end.actual.position.x==0 &&
        end.actual.activity==RoomActivity::resting && end.updated==now && room.start_move(binding,resident.body,1,RoomPlace::window,now).record->action==id &&
        models==0,"wrong arrival, duplicate action or idle model wake");
    return end.actual.revision;
}

// 在同一实例核对多段路线、物理阻路、来源隔离及回执窗口。
// 步骤：关闭通道核对受阻 → 重新开放并经过书桌 → 检查旧实例和旧命令均不能再次执行。
void routes() {
    BuiltinRoom room, other;
    const auto binding=room.layout().room;
    const auto resident=room.self(binding).value();
    const auto t=agent::ObservationTime{};
    check(room.set_passage_open(binding,RoomPlace::window,RoomPlace::desk,false),"known passage not closed");
    const auto blocked=room.start_move(binding,resident.body,10,RoomPlace::window,t);
    check(blocked.admission==MoveAdmission::accepted && blocked.record->phase==MovePhase::blocked &&
        blocked.record->actual.position.x==4 && !room.walking(),"blocked work invented movement");
    check(room.set_passage_open(binding,RoomPlace::desk,RoomPlace::window,true) &&
        room.start_move(binding,resident.body,10,RoomPlace::window,t).record->phase==MovePhase::blocked,"reopening replayed blocked work");
    const auto window=room.start_move(binding,resident.body,11,RoomPlace::window,t).record.value();
    room.advance_move(t+1s);
    const auto screen=room.start_move(binding,resident.body,12,RoomPlace::behind_screen,t+1s).record.value();
    room.advance_move(t+2s);
    check(room.self(binding)->place==RoomPlace::desk && room.walking(),"two-segment route skipped desk");
    room.advance_move(t+3s);
    const auto arrived=room.movement(binding,resident.body,screen.action).value();
    check(arrived.phase==MovePhase::arrived && arrived.actual.position.y==3 && arrived.updated==t+2750ms &&
        room.movement(binding,resident.body,window.action)->actual.place==RoomPlace::window &&
        !room.movement(other.layout().room,resident.body,window.action),"old or foreign arrival crossed action");
    for (unsigned command=13;command<46;++command)
        check(room.start_move(binding,resident.body,command,RoomPlace::behind_screen,t+3s).record->phase==MovePhase::arrived,
            "stationary move did not verify location");
    check(room.start_move(binding,resident.body,10,RoomPlace::window,t+3s).admission==MoveAdmission::stale &&
        !room.movement(binding,resident.body,blocked.record->action),"retired command moved again");
}
}

// 用两个可控时钟节奏和真实应用入口验收原生移动，不调用真实模型。
int main(int argc, char** argv) {
    try {
        if (argc!=2) return 2;
        check(drive(argv[1],100ms)==drive(argv[1],500ms),"scheduling changed body revision");
        routes();
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
