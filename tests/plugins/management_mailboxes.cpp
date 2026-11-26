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

// 区分实际目录错误和测试等待超时。
void check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }

// 在受控目录中等待原生服务发布所需的状态。
template<class Ready> void until(Ready ready, const char* stage) {
    const auto end = std::chrono::steady_clock::now()+30s;
    while (!ready()) {
        if (std::chrono::steady_clock::now()>=end) throw std::runtime_error(std::string("mailbox fixture timed out: ")+stage);
        std::this_thread::sleep_for(10ms);
    }
}

// 建立可通过正式插件命令加载的 C DLL 包。
void package(const fs::path& dir, const fs::path& dll) {
    fs::create_directories(dir);
    fs::copy_file(dll,dir/"entry.dll");
    std::ofstream(dir/"plugin.json") << R"({"manifest_version":1,"id":"test.mail","name":"测试信箱","version":"1","abi_version":2,"entry":"entry.dll","default_config":"defaults.json"})";
    std::ofstream(dir/"defaults.json") << R"({"mode":"t"})";
}

// 使用正式管道、两个客户端和真实 DLL 核对目录修订与实例代次。
// 步骤：建立空插件基线 → 加载并多窗查询 → 卸载、重载并核对来源隔离。
void directory(const fs::path& root, const fs::path& dll) {
    const auto data=root/"user", packages=root/"plugins";
    package(packages/"mail",dll);
    std::atomic_uint model_calls{0};
    int outcome = -1;
    // 在独立线程运行正式后台，固定响应计数器记录所有实际模型调用。
    std::jthread server([&](std::stop_token stop) {
        outcome = management::serve_resident(data,"offline",[&] {
            return std::make_unique<app::Application>(model::Config{},packages,data,model::Backend{[&](const auto&,const auto&) {
                ++model_calls;
                return model::Response{{model::Role::assistant,"fixed"}};
            }});
        },stop);
    });
    until([&] {
        try { return management::resident_request(data,{{"op","status"}}).at("state")=="running"; }
        catch (const std::system_error&) { return false; }
    },"resident startup");
    const auto first=management::connect_resident(data,"offline"), second=management::connect_resident(data,"offline");
    const auto call = [&](const std::string& client, Json body) {
        body.update(Json{{"version","1"},{"request","sample"}});
        return management::resident_request(data,{{"op","request"},{"client",client},{"body",std::move(body)}});
    };
    const Json list{{"type","mailbox.list"}};
    const auto initial=call(first.client,list);
    check(initial.at("instance")==first.instance && initial.at("event")=="mailbox.directory" && initial.at("state")=="ready" &&
        initial.at("sources").size()==2 && initial==call(second.client,list),"missing or unstable built-in directory");
    const auto foreign=call(second.client,{{"type","mailbox.list"},{"instance","previous-backend"}});
    check(foreign.at("state")=="stale" && foreign.at("sources").empty(),"old host exposed the current directory");

    unsigned sequence=0;
    const auto command = [&](const std::string& line) {
        const Json request{{"op","terminal"},{"client",first.client},{"body",{{"id",++sequence},{"action","line"},{"text",line}}}};
        Json result;
        until([&] { result=management::resident_request(data,request); return !result.is_null(); },line.c_str());
        if (result.at("err")!="" || result.at("out")=="") throw std::runtime_error(line+" "+result.dump());
    };
    command("/plugins load test.mail");
    Json loaded;
    until([&] { loaded=call(first.client,list); return loaded.at("sources").size()==3; },"directory after load");
    const auto original=loaded.at("sources").back();
    check(original.at("kind")=="plugin" && original.at("plugin")=="test.mail" && original.at("generation").is_string() &&
        loaded.at("revision")!=initial.at("revision") && loaded==call(second.client,list),"plugin not bound to its original generation");
    command("/plugins unload test.mail");
    until([&] { return call(first.client,list).at("sources").size()==2; },"directory after unload");
    command("/plugins load test.mail");
    Json reloaded;
    until([&] { reloaded=call(second.client,list); return reloaded.at("sources").size()==3; },"directory after reload");
    const auto replacement=reloaded.at("sources").back();
    check(replacement.at("id")!=original.at("id") && replacement.at("generation")!=original.at("generation") &&
        reloaded.at("revision")!=loaded.at("revision") && model_calls==0,"reloaded source reused its identity or viewing started a turn");
    server.request_stop();
    server.join();
    check(outcome==0,"resident failed to drain");
}
}

// 在每次运行专用的目录中验证管理信箱，不使用真实模型。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc!=3) return 2;
        const auto root=fs::path(argv[2])/std::to_string(GetCurrentProcessId())/std::to_string(GetTickCount64());
        directory(root,argv[1]);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
