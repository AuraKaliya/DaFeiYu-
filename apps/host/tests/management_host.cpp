#include "../../../tests/decision_input.hpp"
#include "management/host.hpp"
#include "management/protocol.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <semaphore>

namespace {
using namespace std::chrono_literals;
using namespace dfy;

// 有界等待真实业务线程完成检查所需的推进。
template<class Predicate>
void until(Predicate ready, const char* message) {
    const auto limit = std::chrono::steady_clock::now()+3s;
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= limit) throw std::runtime_error(message);
        std::this_thread::sleep_for(5ms);
    }
}

// 在无窗口时完成用户回合，新观察者能取得原回合、正文及实际退出。
// 步骤：打开空插件宿主 → 提交后释放观察快照 → 放行固定答复 → 验证重读、幂等及显式关闭。
void background_turn(const std::filesystem::path& root) {
    std::filesystem::create_directories(root/"plugins");
    std::binary_semaphore entered(0), release(0);
    std::atomic<int> calls = 0;
    management::Host host([&] {
        return std::make_unique<app::Application>(model::Config{},root/"plugins",root/"user",model::Backend{
            [&](const auto&, const auto& input) {
                ++calls;
                entered.release();
                if (!release.try_acquire_for(4s)) throw std::runtime_error("fixture release timed out");
                return model::Response{{model::Role::assistant,"已收到："+dfy::test::business_messages(input).back().content}};
            }});
    });
    until([&] { return host.dialogue().accepting; },"host did not start");
    const management::DialogueCommand command{1,management::DialogueAction::send,{},"在后台处理",0};
    if (host.submit_dialogue(command).state != management::DialogueCommandState::queued || !entered.try_acquire_for(3s))
        throw std::runtime_error("turn did not enter the model fixture");
    until([&] {
        const auto first = host.dialogue(1);
        return first.display && first.display->turns.size() == 1 && first.receipt &&
            first.receipt->state == management::DialogueCommandState::accepted;
    },"original receipt missing");

    // 一个窗口持续提交忙时命令，只能占用自己的历史回执额度。
    const auto other = host.open_client();
    for (std::uint64_t id=1; id<=20; ++id) {
        const management::DialogueCommand attempt{id,management::DialogueAction::send,{},"忙时普通发送",0,other};
        if (host.submit_dialogue(attempt).state!=management::DialogueCommandState::busy)
            throw std::runtime_error("busy observer submitted a new turn");
    }
    const auto original = host.dialogue(1).receipt, retired = host.dialogue(1,other).receipt;
    if (!original || original->state!=management::DialogueCommandState::accepted ||
        !retired || retired->state!=management::DialogueCommandState::expired)
        throw std::runtime_error("one observer evicted another observer's original receipt");
    host.close_client(other);

    release.release();
    until([&] { return host.dialogue().accepting; },"background turn did not exit");
    const auto later = host.dialogue(1);
    const auto& turn = later.display->turns.at(0);
    if (!later.receipt || turn.run != later.receipt->run || turn.answer != "已收到：在后台处理" ||
        turn.result != runtime::OperationState::succeeded || turn.exit != runtime::OperationState::succeeded ||
        host.submit_dialogue(command).run != turn.run || calls != 1)
        throw std::runtime_error("observer did not recover the original completed turn");

    const auto room = management::dispatch(&host,{{"version","1"},{"request","position"},{"type","room.read"}});
    const auto view = management::dispatch(&host,{{"version","1"},{"request","chat"},{"type","dialogue.read"},{"revision",""},{"command","1"}});
    const auto status = management::dispatch(&host,{{"version","1"},{"request","status"},{"type","state.read"},{"topic","run"}});
    if (room.at("room").at("placeName") != "书桌旁" || view.at("turns").at(0).at("answer") != turn.answer ||
        status.at("run").at("exit") != "succeeded" || status.dump().find(turn.answer) != std::string::npos)
        throw std::runtime_error("management protocol exposed the wrong projection");

    const auto first_client = host.open_client(), second_client = host.open_client();
    const nlohmann::json same{{"type","dialogue.submit"},{"version","1"},{"request","send"},{"command","1"},{"text",command.text}};
    for (const auto& client : {first_client,second_client}) {
        if (host.dialogue(0,client).latest_command != 0 ||
            management::dispatch(&host,same,client).at("receipt").at("state") != "queued" || !entered.try_acquire_for(3s))
            throw std::runtime_error("independent client command was merged");
        host.close_client(client);
        release.release();
        until([&] { return host.dialogue().accepting; },"detached work did not exit");
        if (management::dispatch(&host,same,client).at("receipt").at("state") != "unavailable")
            throw std::runtime_error("closed client could submit");
    }
    if (calls!=3 || host.dialogue().display->turns.size()!=3) throw std::runtime_error("independent messages lost");

    host.close();
    until([&] { return host.exited(); },"explicit shutdown did not finish");
    if (host.failed() || host.dialogue().accepting) throw std::runtime_error("host did not close cleanly");
}

// 在宿主消费已接纳命令前撤销客户端，命令仍交给原业务线程一次。
// 步骤：暂停已就绪的宿主 → 投递并立即断开 → 放行业务 → 新观察者核对原消息与实际退出。
void detach_before_apply(const std::filesystem::path& root) {
    std::atomic_bool hold = false;
    std::binary_semaphore entered(0), release(0);
    management::Host host([&] {
        return std::make_unique<app::Application>(model::Config{},root/"plugins",root/"detach",model::Backend{
            [](const auto&,const auto&) { return model::Response{{model::Role::assistant,"已办理"}}; }});
    },[&](auto&) {
        if (!hold.exchange(false)) return;
        entered.release();
        if (!release.try_acquire_for(3s)) throw std::runtime_error("host advance gate timed out");
    });
    until([&] { return host.dialogue().accepting; },"detachment host unavailable");
    hold = true;
    if (!entered.try_acquire_for(3s)) throw std::runtime_error("host advance was not paused");
    const auto client = host.open_client();
    const auto queued = host.submit_dialogue({1,management::DialogueAction::send,{},"关窗前已提交",0,client});
    host.close_client(client);
    release.release();
    if (queued.state!=management::DialogueCommandState::queued) throw std::runtime_error("command was not queued");

    const auto observer = host.open_client();
    until([&] {
        const auto snapshot = host.dialogue(0,observer);
        if (!snapshot.accepting || snapshot.display->turns.size()!=1) return false;
        const auto& turn = snapshot.display->turns.front();
        return turn.prompt=="关窗前已提交" && turn.answer=="已办理" && turn.exit==runtime::OperationState::succeeded;
    },"detached command lost before application admission");
    host.close();
    until([&] { return host.exited(); },"detachment host did not exit");
    if (host.failed()) throw std::runtime_error("detachment host failed");
}

// 在业务线程发起原生移动，验证正式窗口协议可读取在途和实际抵达的位置。
// 步骤：固定可控时钟并起步 → 读取半程坐标和身体活动 → 推进抵达并核对同一身体。
void room_motion_snapshot(const std::filesystem::path& root) {
    std::atomic<int> ms=500;
    management::Host host([&] {
        app::ApplicationRunConfig config;
        config.tool_clock=[&] { return agent::ObservationTime{}+std::chrono::milliseconds(ms.load()); };
        auto application=std::make_unique<app::Application>(model::Config{},root/"plugins",root/"room-motion",model::Backend{
            [](const auto&,const auto&) { return model::Response{{model::Role::assistant,"固定"}}; }},config);
        auto& room=application->room();
        const auto binding=room.layout().room;
        const auto started=room.start_move(binding,room.self(binding)->body,1,app::RoomPlace::window,agent::ObservationTime{});
        if (started.admission!=app::MoveAdmission::accepted) throw std::runtime_error("motion fixture did not start");
        return application;
    });
    const auto read=[&] { return management::dispatch(&host,{{"version","1"},{"request","position"},{"type","room.read"}}).at("room"); };
    until([&] { const auto room=read(); return room.is_object() && room.at("activity")=="walking" && room.at("x")==2; },"window missed transit");
    const auto body=read().at("body");
    ms=1000;
    until([&] { const auto room=read(); return room.at("activity")=="resting" && room.at("placeName")=="窗边" &&
        room.at("x")==0 && room.at("body")==body; },"window missed arrival");
    host.close();
    until([&] { return host.exited(); },"room motion host did not exit");
}
}

// 在 CTest 分配的目录运行无窗口业务生命周期回归。
int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        background_turn(argv[1]);
        detach_before_apply(argv[1]);
        room_motion_snapshot(argv[1]);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
