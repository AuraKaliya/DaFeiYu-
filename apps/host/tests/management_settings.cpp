#include "management/resident.hpp"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <atomic>
#include <iostream>

namespace {
using namespace dfy;
using namespace std::chrono_literals;
using Json = nlohmann::json;
namespace fs = std::filesystem;

// 将局部协议断言转成明确的测试失败。
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

// 经真实命名管道用当前已握手的客户端提交设置请求。
Json call(const fs::path& data, const std::string& client, Json body) {
    body.update(Json{{"version","1"},{"request","settings-check"}});
    return management::resident_request(data,{{"op","request"},{"client",client},{"body",std::move(body)}});
}

// 用隔离目录、固定模型与两个真实连接运行同一后台，结束时核对实际退出。
// 步骤：启动带已知供应商和超时值的应用 → 等待就绪 → 连接两个窗口 → 执行场景并停止。
template<class Exercise> void with_resident(const fs::path& data, Exercise exercise) {
    std::atomic_uint calls{0};
    int result = -1;
    std::jthread server([&](std::stop_token stop) {
        result = management::serve_resident(data,"settings-test",[&] {
            model::Config config{"qwen","private-endpoint","private-model","PRIVATE_ENV_SENTINEL",1357};
            return std::make_unique<app::Application>(config,data/"packages",data,model::Backend{
                [&](const auto&,const auto&) { ++calls; return model::Response{{model::Role::assistant,"fixed"}}; }});
        },stop);
    });
    const auto end = std::chrono::steady_clock::now()+25s;
    for (;;) {
        try { if (management::resident_request(data,{{"op","status"}}).at("state")=="running") break; }
        catch (const std::system_error&) {}
        check(std::chrono::steady_clock::now()<end,"settings resident startup timed out");
        std::this_thread::sleep_for(10ms);
    }
    const auto a = management::connect_resident(data,"settings-test"), b = management::connect_resident(data,"settings-test");
    exercise(a,b);
    server.request_stop();
    server.join();
    check(!result && !calls,"settings access ran a model or resident failed to exit");
}

// 验证两个窗口只读同一主体配置，过期或未授权的请求不能取得字段。
void read_settings(const fs::path& data) {
    with_resident(data,[&](const auto& one,const auto& two) {
        Json read{{"type","settings.read"},{"owner","host"},{"instance",one.instance}};
        const auto view=call(data,one.client,read);
        const auto& fields=view.at("fields");
        check(view.at("state")=="ready" && view.at("event")=="settings.snapshot" && view.at("owner")=="host" &&
            view.at("instance")==one.instance && view.at("appliedRevision")==view.at("revision") &&
            view.at("pendingRestart")==false && fields.size()==2,"initial settings scope or revisions differ");
        check(fields.at(0).at("id")=="provider" && fields.at(0).at("access_mode")=="read_only" && fields.at(0).at("value")=="qwen" &&
            fields.at(1).at("id")=="timeout_ms" && fields.at(1).at("type")=="integer" && fields.at(1).at("value")==1357 &&
            fields.at(1).at("appliedValue")==1357 && fields.at(1).at("source")=="startup","settings did not reflect the running application");
        read["revision"]=view.at("revision");
        check(view==call(data,two.client,read) && view.dump().find("private-")==std::string::npos &&
            view.dump().find("PRIVATE_ENV_SENTINEL")==std::string::npos,"read changed state or included private model configuration");
        read["revision"]="0";
        check(call(data,one.client,read).at("state")=="stale","obsolete settings revision was accepted");
        read.erase("revision");
        read["instance"]="old-resident";
        const auto stale=call(data,two.client,read);
        check(stale.at("state")=="stale" && stale.at("fields").empty(),"old instance disclosed settings");
        bool refused=false;
        try { (void)call(data,"unregistered",read); } catch (const std::runtime_error&) { refused=true; }
        check(refused,"client without a handshake reached settings");
    });
}
}

// 在独立用户数据目录验证设置读写入口，测试使用受控本地模型替身。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc!=2) return 2;
        const auto data=fs::path(argv[1])/std::to_string(GetCurrentProcessId())/std::to_string(GetTickCount64());
        fs::create_directories(data/"packages");
        read_settings(data);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
