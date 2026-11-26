#include "management/resident.hpp"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <atomic>
#include <fstream>
#include <future>
#include <iostream>
#include <latch>

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
template<class Exercise> void with_resident(const fs::path& data, Exercise exercise, int probe_timeout = 0) {
    std::atomic_uint calls{0};
    std::atomic_int used_timeout{0};
    int result = -1;
    std::jthread server([&](std::stop_token stop) {
        result = management::serve_resident(data,"settings-test",[&] {
            auto config = model::load_config(data/"startup.json");
            app::ApplicationRunConfig run;
            run.manage_host_settings = true;
            return std::make_unique<app::Application>(config,data/"packages",data,model::Backend{
                [&](const auto& actual,const auto&) {
                    used_timeout=actual.timeout_ms;
                    ++calls;
                    return model::Response{{model::Role::assistant,"fixed"}};
                }},run);
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
    check(!calls,"settings access started a model");
    if (probe_timeout) {
        const auto accepted=call(data,a.client,{{"type","dialogue.submit"},{"command","1"},{"text","timeout probe"},
            {"submission",a.instance+"/timeout-probe"}});
        check(accepted.at("receipt").at("state")=="queued","controlled timeout probe was not admitted");
        const auto deadline=std::chrono::steady_clock::now()+15s;
        while (!calls && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(10ms);
        check(calls==1 && used_timeout==probe_timeout,"controlled backend received a different runtime timeout");
    }
    server.request_stop();
    server.join();
    check(!result && calls==(probe_timeout ? 1u : 0u),"resident failed to exit or invoked extra model work");
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

// 逐字节读取夹具文件，核对失败写入与独立配置均保留原内容。
std::string contents(const fs::path& file) {
    std::ifstream input(file,std::ios::binary);
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}

// 从原主体设置视图形成一次独立、可查证的提交。
Json save_intent(const Json& view, std::string submission, Json changes) {
    return {{"type","settings.save"},{"owner","host"},{"instance",view.at("instance")},{"revision",view.at("revision")},
        {"submission",std::move(submission)},{"changes",std::move(changes)}};
}

// 使用两个窗口检验同修订并发保存、磁盘失败保护及重启后的实际模型参数。
// 步骤：直接绕过界面提交只读字段 → 竞争同一修订 → 锁定文件制造发布失败 → 重启并核对原提交。
void write_and_restart(const fs::path& data) {
    const auto file=data/"host-settings.json", plugin_file=data/"plugins"/"test.unrelated"/"settings.json";
    fs::create_directories(plugin_file.parent_path());
    std::ofstream(plugin_file) << R"({"displayName":"untouched"})";
    const auto plugin_original=contents(plugin_file), startup_original=contents(data/"startup.json");
    Json winner;
    std::string old_instance;
    with_resident(data,[&](const auto& a,const auto& b) {
        old_instance=a.instance;
        const Json read{{"type","settings.read"},{"owner","host"},{"instance",a.instance}};
        const auto initial=call(data,a.client,read);
        check(initial.at("canSave")==true && !fs::exists(file),"reading initialized the user file or failed to enable a managed save");
        const auto mixed=save_intent(initial,"mixed",{{"provider","deepseek"},{"timeout_ms",2000}});
        check(call(data,a.client,mixed).at("state")=="readOnly","read-only provider was writable through the native service");
        for (const Json value : {Json(0),Json(600001),Json("2000"),Json(2.0)}) {
            const auto bad=save_intent(initial,"invalid",{{"timeout_ms",value}});
            check(call(data,b.client,bad).at("state")=="invalid","invalid timeout accepted");
        }
        check(!fs::exists(file) && call(data,b.client,read)==initial,"rejected request wrote a file or changed a value");

        std::latch start(2);
        const auto race=[&](const std::string& client,std::string submission,int timeout) {
            auto intent=save_intent(initial,std::move(submission),{{"timeout_ms",timeout}});
            start.arrive_and_wait();
            return call(data,client,std::move(intent));
        };
        auto other=std::async(std::launch::async,race,b.client,"race-b",4100);
        const auto first=race(a.client,"race-a",2700), second=other.get();
        winner=first.at("state")=="saved" ? first : second;
        const auto& loser=first.at("state")=="saved" ? second : first;
        check(winner.at("state")=="saved" && loser.at("state")=="conflict" && winner.at("savedRevision")=="2" &&
            loser.at("revision")==winner.at("savedRevision"),"same-revision writers both succeeded or lost the latest revision");
        const auto changed=call(data,b.client,read);
        check(changed.at("revision")=="2" && changed.at("appliedRevision")=="1" && changed.at("pendingRestart")==true &&
            changed.at("fields").at(1).at("value")==winner.at("savedValue") && changed.at("fields").at(1).at("appliedValue")==1357 &&
            changed.at("fields").at(1).at("source")=="user","saved timeout replaced the running value");
        const auto original=save_intent(initial,winner.at("submission").get<std::string>(),{{"timeout_ms",winner.at("savedValue")}});
        check(call(data,b.client,original)==winner,"retry of the same submission created a second write");
        auto altered=original;
        altered["changes"]["timeout_ms"]=11;
        check(call(data,b.client,altered).at("state")=="submissionConflict","changed payload reused an original submission");

        const auto before=contents(file);
        const auto lock=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        check(lock!=INVALID_HANDLE_VALUE,"could not hold native replacement lock");
        const auto failed=call(data,a.client,save_intent(changed,"locked",{{"timeout_ms",5800}}));
        CloseHandle(lock);
        check(failed.at("state")=="failed" && !failed.contains("savedRevision") && contents(file)==before &&
            call(data,b.client,read)==changed,"failed replacement changed the original bytes, value or revision");
        for (const auto& entry : fs::directory_iterator(data))
            check(!entry.path().filename().native().starts_with(L"host-settings.json.tmp-"),"failed save left a temporary user file");
        const auto final=call(data,b.client,save_intent(changed,"retry-after-lock",{{"timeout_ms",5800}}));
        check(final.at("state")=="saved" && final.at("savedRevision")=="3" && final.at("appliedRevision")=="1",
            "new save did not preserve the startup revision");
    },1357);

    with_resident(data,[&](const auto& a,const auto& b) {
        Json read{{"type","settings.read"},{"owner","host"},{"instance",old_instance}};
        check(a.instance!=old_instance && call(data,a.client,read).at("state")=="stale","old instance attached to newly applied settings");
        read["instance"]=a.instance;
        const auto view=call(data,a.client,read);
        check(view.at("revision")=="3" && view.at("appliedRevision")=="3" && view.at("pendingRestart")==false &&
            view.at("fields").at(1).at("value")==5800 && view.at("fields").at(1).at("appliedValue")==5800,"restart lost the saved timeout");
        const Json query{{"type","settings.receipt"},{"owner","host"},{"instance",a.instance},{"submission",winner.at("submission")}};
        const auto receipt=call(data,b.client,query);
        check(receipt.at("savedRevision")=="2" && receipt.at("savedValue")==winner.at("savedValue") && receipt.at("revision")=="3",
            "reopened window lost the original durable receipt");
    },5800);
    check(contents(plugin_file)==plugin_original && contents(data/"startup.json")==startup_original,"saving host settings modified another owner");
}
}

// 在独立用户数据目录验证设置读写入口，测试使用受控本地模型替身。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc!=2) return 2;
        const auto data=fs::path(argv[1])/std::to_string(GetCurrentProcessId())/std::to_string(GetTickCount64());
        fs::create_directories(data/"packages");
        std::ofstream(data/"startup.json") << R"({"provider":"qwen","base_url":"https://private-endpoint.test/v1","model":"private-model","api_key_env":"PRIVATE_ENV_SENTINEL","timeout_ms":1357})";
        read_settings(data);
        write_and_restart(data);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
