#include "management/resident.hpp"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <atomic>
#include <fstream>
#include <iostream>

namespace {
using namespace dfy;
using namespace std::chrono_literals;
using Json = nlohmann::json;
namespace fs = std::filesystem;

// 将原生回执与期望状态的偏差报告为测试失败。
void check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }

// 有界等待常驻端点或业务线程发布目标状态。
template<class Ready> void until(Ready ready, const char* stage) {
    const auto end = std::chrono::steady_clock::now()+30s;
    do {
        if (ready()) return;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now()<end);
    throw std::runtime_error(std::string("plugin fixture timed out: ")+stage);
}

// 创建待安装的真实 C ABI 插件包。
void package(const fs::path& source, const fs::path& dll) {
    fs::create_directories(source);
    fs::copy_file(dll,source/"entry.dll");
    std::ofstream(source/"plugin.json") << R"({"manifest_version":1,"id":"test.directory","name":"目录测试","version":"1","abi_version":2,"entry":"entry.dll"})";
}

// 通过真实端点以原观察者身份提交一个管理请求。
Json call(const fs::path& data, const std::string& client, Json body) {
    body.update(Json{{"version","1"},{"request","check"}});
    return management::resident_request(data,{{"op","request"},{"client",client},{"body",std::move(body)}});
}

// 从窗口看到的原目录行生成受后台、修订和代次约束的命令。
Json intent(const Json& directory, const Json& row, const char* action, unsigned sequence) {
    return {{"type",std::string("plugins.")+action},{"instance",directory.at("instance")},{"command",std::to_string(sequence)},
        {"id",row.at("id")},{"revision",directory.at("revision")},{"generation",row.at("generation")}};
}

// 使用原操作号从任一有效连接读回终态，等待期间不重发写命令。
Json settled(const fs::path& data, const std::string& client, const Json& submitted) {
    const Json read{{"type","plugins.receipt"},{"instance",submitted.at("instance")},{"operation",submitted.at("operation")}};
    Json result;
    until([&] {
        result=call(data,client,read);
        const auto& state=result.at("state");
        return state!="queued" && state!="running" && state!="pending";
    },"original plugin operation");
    return result;
}

// 用同号轮询确认终端插件命令实际执行，失败保留原回执。
void terminal(const fs::path& data, const std::string& client, unsigned id, const std::string& line) {
    const Json envelope{{"op","terminal"},{"client",client},{"body",{{"id",id},{"action","line"},{"text",line}}}};
    Json result;
    until([&] { result=management::resident_request(data,envelope); return !result.is_null(); },"terminal reply");
    if (result.at("err")!="" || result.at("out")=="") throw std::runtime_error(line+" "+result.dump());
}

// 通过两个实名管道客户端核对磁盘包、独立运行代次和未被消费的到件。
// 步骤：观察空目录 → 正式安装、加载及双窗复查 → 卸载与重载 → 核对旧实例和旧修订。
void directory(const fs::path& root, const fs::path& dll) {
    const auto data=root/"user", packages=root/"packages", source=root/"source";
    fs::create_directories(data);
    fs::create_directories(packages);
    package(source,dll);
    std::atomic_uint model_calls{0};
    int outcome = -1;
    // 后台启动时留下一封无自动唤醒的原生来信，供测试复查真实消费水位。
    std::jthread server([&](std::stop_token stop) {
        outcome=management::serve_resident(data,"offline",[&] {
            auto app=std::make_unique<app::Application>(model::Config{},packages,data,model::Backend{
                [&](const auto&,const auto&) { ++model_calls; return model::Response{{model::Role::assistant,"fixed"}}; }});
            app->post_room_event(app::RoomEvent::presence_changed,"书房","pending evidence");
            return app;
        },stop);
    });
    until([&] {
        try { return management::resident_request(data,{{"op","status"}}).at("state")=="running"; }
        catch (const std::system_error&) { return false; }
    },"resident startup");
    const auto one=management::connect_resident(data,"offline"), two=management::connect_resident(data,"offline");
    const Json list{{"type","plugins.list"}};
    const auto read=[&](const auto& client) { return call(data,client,list); };
    const auto first=read(one.client);
    check(first.at("event")=="plugins.directory" && first.at("instance")==one.instance && first.at("state")=="ready" &&
        first.at("packages").empty() && first==read(two.client),"empty package catalog was missing or unstable");
    const Json room{{"type","mailbox.read"},{"instance",one.instance},{"source","builtin/room"}};
    const auto pending=call(data,one.client,room);
    check(pending.at("messages").at(0).at("state")=="pending","fixture evidence was not pending");

    const auto path=source.generic_u8string();
    terminal(data,one.client,1,"/plugins install \""+std::string(path.begin(),path.end())+"\"");
    Json installed;
    until([&] { installed=read(two.client); return installed.at("packages").size()==1; },"installed catalog");
    const auto row=installed.at("packages").front();
    check(row.at("id")=="test.directory" && row.at("name")=="目录测试" && row.at("version")=="1" && row.at("state")=="installed" &&
        row.at("generation")=="0" && row.at("installed")==true && row.at("loaded")==false && row.at("canLoad")==true &&
        row.at("canUnload")==false && installed.at("revision")!=first.at("revision"),"installation fabricated a running instance");
    const auto load=intent(installed,row,"load",1);
    bool extra_refused=false, outsider_refused=false;
    try { auto path_argument=load; path_argument["path"]="outside"; (void)call(data,one.client,path_argument); }
    catch (const std::runtime_error&) { extra_refused=true; }
    try { (void)call(data,"not-a-connected-client",load); }
    catch (const std::runtime_error&) { outsider_refused=true; }
    check(extra_refused && outsider_refused && read(two.client)==installed,"unbound client or filesystem argument reached a load");
    auto skipped=load;
    skipped["command"]="2";
    const auto out_of_order=call(data,one.client,skipped);
    check(out_of_order.at("state")=="outOfOrder" && out_of_order.at("expectedCommand")=="1","command sequence gap executed");
    const auto admitted=call(data,one.client,load);
    check(admitted.at("event")=="plugins.operation" && admitted.at("state")=="queued" && admitted.at("effect")=="unconfirmed",
        "admission claimed an unverified load");
    const auto loaded=settled(data,two.client,admitted);
    check(loaded.at("state")=="succeeded" && loaded.at("effect")=="loaded" && loaded.at("nativeCode")==DFY_V2_OK &&
        loaded.at("generation")!="0" && call(data,one.client,load)==loaded,"original load receipt was lost or re-executed");
    auto conflict=load;
    conflict["id"]="test.other";
    check(call(data,one.client,conflict).at("state")=="conflict","same command accepted a different target");
    const auto obsolete=settled(data,one.client,call(data,two.client,load));
    check(obsolete.at("state")=="stale" && obsolete.at("effect")=="notApplied","second window reloaded from an obsolete sample");
    Json active;
    until([&] { active=read(one.client); return active.at("packages").front().at("state")=="active"; },"active generation");
    const auto live=active.at("packages").front();
    const Json pinned{{"type","plugins.list"},{"instance",one.instance},{"revision",active.at("revision")}};
    check(live.at("loaded")==true && live.at("generation")==loaded.at("generation") && live.at("canLoad")==false && live.at("canUnload")==true &&
        live.at("loadedVersion")=="1" && active==call(data,two.client,pinned),"inconsistent active generation");
    terminal(data,one.client,3,"/plugins refresh");
    check(call(data,one.client,pinned)==active && read(two.client)==active && call(data,two.client,room)==pending && !model_calls,
        "read or unchanged catalog refresh changed the revision, consumed an event or ran the model");
    terminal(data,one.client,4,"/plugins unload test.directory");
    until([&] { return read(one.client).at("packages").front().at("state")=="installed"; },"actual unload");
    check(call(data,two.client,pinned).at("state")=="stale","old package revision was accepted");
    terminal(data,one.client,5,"/plugins load test.directory");
    until([&] { const auto current=read(two.client).at("packages").front();
        return current.at("state")=="active" && current.at("generation")!=live.at("generation"); },"replacement generation");
    const auto old=call(data,one.client,{{"type","plugins.list"},{"instance","previous-resident"}});
    check(old.at("state")=="stale" && old.at("packages").empty() && !model_calls,"old backend disclosed packages");
    server.request_stop();
    server.join();
    check(!outcome,"resident did not exit normally");
}
}

// 为每次运行隔离插件包、命名管道和用户设置，以固定模型验证管理入口。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc!=3) return 2;
        const auto root=fs::path(argv[2])/std::to_string(GetCurrentProcessId())/std::to_string(GetTickCount64());
        directory(root,argv[1]);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
