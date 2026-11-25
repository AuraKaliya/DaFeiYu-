#include "application.hpp"
#include "main_messages.hpp"
#include "management/resident.hpp"
#include "management/protocol.hpp"
#include "management/local_pipe.hpp"
#include <winsqlite/winsqlite3.h>
#include <windows.h>
#include <iostream>
#include <thread>
#include <semaphore>

namespace {
using namespace dfy;
using Json = nlohmann::json;
// 推进真实应用直到指定的持久事实出现，模型由同步计数的固定替身提供。
template<class Predicate>
void until(app::Application& application, Predicate done) {
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(12);
    while (!done()) {
        application.advance_plugins();
        application.conversation().advance();
        if (std::chrono::steady_clock::now()>end) throw std::runtime_error("recovery timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
// 由另一条系统 SQLite 连接阻塞写入，测试作用域结束后解除。
struct WriteLock {
    sqlite3* db = nullptr;
    // 读取相同文件并取得保留写锁，主连接仍可观察旧水位。
    explicit WriteLock(const std::filesystem::path& profile) {
        const auto name = (profile/"main-conversation.sqlite3").u8string();
        if (sqlite3_open(reinterpret_cast<const char*>(name.c_str()),&db)!=SQLITE_OK ||
            sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)!=SQLITE_OK) throw std::runtime_error("cannot acquire fixture write lock");
    }
    // 退出未提交的测试事务。
    ~WriteLock() { sqlite3_close(db); }
};

// 管道已确认原文后阻塞业务写盘，同一窗口和重连窗口都必须能查回最终接纳。
// 步骤：暂停真实业务线程 → 从管道提交并加写锁 → 同号查回等待 → 解锁后复查两窗及调用数。
void command_recovery(const std::filesystem::path& data) {
    auto db = std::make_shared<app::MessageStore>(data);
    std::atomic_uint calls{0}, ticks{0};
    std::atomic_bool pause{false};
    std::binary_semaphore parked(0), resume(0);
    management::Host host([&] {
        return std::make_unique<app::Application>(model::Config{},data/"empty",data,model::Backend{[&](const auto&,const auto&) {
            ++calls;
            return model::Response{{model::Role::assistant,"fixed"}};
        }});
    },[&](auto&) {
        if (pause.exchange(false)) { parked.release(); if (!resume.try_acquire_for(std::chrono::seconds(8))) throw std::runtime_error("gate expired"); }
        ++ticks;
    },nullptr,db,"fixture");
    const auto first = host.open_client(), second = host.open_client();
    management::LocalPipe pipe(data);
    std::jthread server([&](std::stop_token stop) {
        while (!stop.stop_requested()) pipe.serve_one(std::chrono::milliseconds(50),[&](const std::string& body) {
            const auto request = Json::parse(body);
            return Json{{"v",1},{"result",management::dispatch(&host,request.at("body"),request.at("client"))}}.dump();
        });
    });
    const auto call = [&](const std::string& client, Json body) {
        body.update(Json{{"request","native"},{"version","1"}});
        return management::resident_request(data,{{"op","request"},{"client",client},{"body",body}}).at("receipt").get<Json>();
    };
    const Json submit{{"type","dialogue.submit"},{"command","1"},{"text","locked original"},{"submission","original"}};
    const Json read{{"type","dialogue.read"},{"command","1"},{"revision",""}};
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while (!host.dialogue().display && std::chrono::steady_clock::now()<end) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    pause = true;
    if (!parked.try_acquire_for(std::chrono::seconds(3))) throw std::runtime_error("worker did not reach admission gate");
    const auto admitted = call(first,submit);
    {
        WriteLock lock(data);
        const auto before = ticks.load();
        resume.release();
        while (ticks<=before+1 && std::chrono::steady_clock::now()<end) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (admitted.at("state")!="queued" || call(first,read).at("state")!="queued" || call(second,submit).at("state")!="queued" || calls)
            throw std::runtime_error("locked command lost its pending identity");
    }
    while (calls==0 && std::chrono::steady_clock::now()<end) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Json receipt;
    do {
        receipt = call(first,read);
        if (receipt.at("state")=="accepted") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    } while (std::chrono::steady_clock::now()<end);
    if (receipt.at("state")!="accepted" || receipt.at("storedMessage")!=admitted.at("storedMessage") ||
        call(first,submit)!=receipt || call(second,read)!=receipt || calls!=1)
        throw std::runtime_error("original command remained queued after durable acceptance");
    for (const auto state : {"cancelled","closed","unknown"}) {
        db->update(std::stoll(admitted.at("storedMessage").get<std::string>()),{{"state",state}});
        if (call(first,read).at("state")=="accepted") throw std::runtime_error("terminal receipt misreported as accepted");
    }
}

// 重开原库时核对四种崩溃边界，之后阻塞真实写盘并核对最终投递次数。
// 步骤：准备不同的已提交状态 → 验证重启归属与原队列 → 验证交付前失败 → 验证输出补写。
void verify(const std::filesystem::path& data) {
    const agent::RunId old_run{3,9};
    {
        app::MessageStore old(data);
        for (const auto& [id,route,state] : {std::tuple{"safe","send","pending"},{"uncertain","send","dispatching"},
            {"stale","append","pending"},{"queued","enqueue","queued"},{"maybe-in-model","append","waiting"}}) {
            const auto envelope = app::user_envelope(route,id,"old",std::string_view(route)=="send" ? agent::RunId{} : old_run);
            old.put("desktop",id,envelope,{{"state",state},{"instance","old"},{"session","3"},{"turn","9"},{"nativeMessage","1"}});
        }
    }
    // 原版本已经保留原文和回执，但尚无待办索引。用同一结构验证真实升级事务。
    {
        sqlite3* legacy = nullptr;
        const auto path = (data/"main-conversation.sqlite3").u8string();
        if (sqlite3_open(reinterpret_cast<const char*>(path.c_str()),&legacy)!=SQLITE_OK) throw std::runtime_error("legacy fixture open failed");
        std::unique_ptr<sqlite3,decltype(&sqlite3_close)> close(legacy,&sqlite3_close);
        if (sqlite3_exec(legacy,"DROP TABLE outbox; PRAGMA user_version=1",nullptr,nullptr,nullptr)!=SQLITE_OK)
            throw std::runtime_error("legacy fixture schema failed");
    }
    auto db = std::make_shared<app::MessageStore>(data);
    std::atomic_uint calls{0};
    app::Application application({},data/"plugins",data,model::Backend{[&](const auto&,const auto&) {
        ++calls;
        return model::Response{{model::Role::assistant,"fixed answer"}};
    }});
    application.bind_messages(db,"new",nullptr);
    const auto& queue = application.conversation().display();
    if (db->find("desktop","uncertain")->receipt.at("state")!="unknown" || db->find("desktop","maybe-in-model")->receipt.at("state")!="unknown" ||
        db->find("desktop","stale")->receipt.at("state")!="stale" || !queue.queue_paused || queue.queue.size()!=1 || calls!=0)
        throw std::runtime_error("restart replayed an unknown effect or lost a queued message");
    until(application,[&] { return calls==1 && !application.conversation().busy(); });
    if (db->find("desktop","safe")->receipt.at("state")!="accepted" || !application.resume_dialogue_queue(queue.queue.front().message))
        throw std::runtime_error("safe pending send or paused queue was lost");
    until(application,[&] { return calls==2 && !application.conversation().busy(); });
    if (db->find("desktop","queued")->receipt.at("state")!="accepted") throw std::runtime_error("resumed queue did not start once");

    const auto envelope = app::user_envelope("send","locked",{}, {});
    const auto original = db->put("desktop","locked",envelope,{{"state","pending"}});
    {
        WriteLock lock(data);
        auto& channel = *application.main_messages();
        if (channel.deliver(original.message.id,envelope).at("state")!="pending" || calls!=2 ||
            db->watermark()!=original.message.revision) throw std::runtime_error("write failure started a model call");
    }
    until(application,[&] { return calls==3 && !application.conversation().busy(); });
    const auto prior = db->watermark();
    application.main_messages()->terminal("out","output while locked");
    {
        WriteLock lock(data);
        until(application,[&] { return calls==4 && !application.conversation().busy(); });
        if (db->page(true,0,db->watermark(),1).records.front().source!="terminal") throw std::runtime_error("failed output write published");
    }
    application.main_messages()->finish();
    if (db->watermark()<=prior || db->page(true,0,db->watermark(),1).records.front().source!="main" || calls!=4 ||
        !db->unfinished(0).empty()) throw std::runtime_error("verified output did not finish writing exactly once");
}
}
// 用独立数据目录验证重开和失败恢复，不连接真实模型。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc!=2) return 2;
        const auto root = std::filesystem::path(argv[1])/std::to_string(GetCurrentProcessId())/std::to_string(GetTickCount64());
        verify(root/"restart");
        command_recovery(root/"window");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
