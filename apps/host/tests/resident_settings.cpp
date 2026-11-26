#include "management/resident.hpp"
#include <windows.h>
#include <fstream>
#include <iostream>

namespace {
using namespace dfy;
using namespace std::chrono_literals;
using Json = nlohmann::json;
namespace fs = std::filesystem;

// 报告正式本体的协议响应与持有的原进程不一致。
void check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }

// 故障时只清理本用例创建且仍在运行的独立后台。
struct Child {
    PROCESS_INFORMATION process{};
    // 归还本用例的线程与进程句柄，正常路径已通过原实例协议退出。
    ~Child() {
        if (process.hThread) CloseHandle(process.hThread);
        if (!process.hProcess) return;
        if (WaitForSingleObject(process.hProcess,0)==WAIT_TIMEOUT) {
            TerminateProcess(process.hProcess,2);
            WaitForSingleObject(process.hProcess,5000);
        }
        CloseHandle(process.hProcess);
    }
};

// 通过已握手的原观察者读取或保存配置。
Json call(const fs::path& data,const std::string& client,Json body) {
    body.update(Json{{"version","1"},{"request","real-host-settings"}});
    return management::resident_request(data,{{"op","request"},{"client",client},{"body",std::move(body)}});
}

// 从正式参数入口启动本体，通过两个连接核对值和原进程的退出码。
// 步骤：隐藏启动指定二进制 → 等待匹配进程就绪 → 核对保存或重启应用 → 停止并等待原进程。
std::string run(const fs::path& binary,const fs::path& root,const std::string& previous) {
    const auto config=root/"offline.json", packages=root/"packages", profile=root/"user data";
    auto args=L"\""+binary.wstring()+L"\" --host \""+config.wstring()+L"\" --plugins \""+packages.wstring()+L"\" --data \""+profile.wstring()+L"\"";
    Child child;
    STARTUPINFOW startup{sizeof(STARTUPINFOW)};
    check(CreateProcessW(binary.c_str(),args.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child.process),
        "could not launch the formal resident");
    const auto deadline=std::chrono::steady_clock::now()+25s;
    for (;;) {
        try {
            const auto state=management::resident_request(profile,{{"op","status"}});
            if (state.at("pid")==child.process.dwProcessId && state.at("state")=="running") break;
        } catch (const std::system_error&) {}
        check(std::chrono::steady_clock::now()<deadline && WaitForSingleObject(child.process.hProcess,20)==WAIT_TIMEOUT,
            "formal resident did not become ready");
    }
    const auto definition=management::startup_identity(config,packages,{});
    const auto one=management::connect_resident(profile,definition), two=management::connect_resident(profile,definition);
    const Json read{{"type","settings.read"},{"owner","host"},{"instance",one.instance}};
    const auto snapshot=call(profile,one.client,read);
    check(snapshot.at("state")=="ready" && snapshot.at("canSave")==true && snapshot==call(profile,two.client,read),
        "formal startup did not publish writable settings consistently");
    if (previous.empty()) {
        check(snapshot.at("fields").at(1).at("appliedValue")==2468 && !fs::exists(profile/"host-settings.json"),
            "formal startup ignored its explicit configuration");
        const auto result=call(profile,one.client,{{"type","settings.save"},{"owner","host"},{"instance",one.instance},
            {"revision",snapshot.at("revision")},{"submission","formal-save"},{"changes",{{"timeout_ms",3690}}}});
        check(result.at("state")=="saved" && result.at("savedRevision")=="2" && result.at("pendingRestart")==true &&
            call(profile,two.client,read).at("fields").at(1).at("appliedValue")==2468,"formal save claimed an immediate runtime change");
    } else {
        auto old=read;
        old["instance"]=previous;
        const auto durable=call(profile,two.client,{{"type","settings.receipt"},{"owner","host"},{"instance",two.instance},{"submission","formal-save"}});
        check(one.instance!=previous && call(profile,two.client,old).at("state")=="stale" && snapshot.at("revision")=="2" &&
            snapshot.at("appliedRevision")=="2" && snapshot.at("pendingRestart")==false && snapshot.at("fields").at(1).at("appliedValue")==3690 &&
            durable.at("savedRevision")=="2" && durable.at("savedValue")==3690,"formal restart did not apply the original saved value");
    }
    const Json state{{"type","state.read"},{"topic","run"}};
    check(call(profile,two.client,state).at("run").is_null(),"formal settings access started a conversation");
    (void)management::request_stop(profile,one.instance,one.client,nullptr);
    DWORD code=1;
    check(WaitForSingleObject(child.process.hProcess,10000)==WAIT_OBJECT_0 && GetExitCodeProcess(child.process.hProcess,&code) && code==0,
        "original resident did not exit normally");
    return one.instance;
}
}

// 以合成配置和空插件目录复查正式后台两次启动均采用正确的主体超时。
int wmain(int argc,wchar_t** argv) {
    try {
        if (argc!=3) return 2;
        const auto root=fs::path(argv[2])/std::to_string(GetCurrentProcessId())/std::to_string(GetTickCount64());
        fs::create_directories(root/"packages");
        std::ofstream(root/"offline.json") << R"({"provider":"qwen","base_url":"https://offline.invalid","model":"fixture","api_key_env":"DFY_SETTINGS_FIXTURE_UNUSED","timeout_ms":2468})";
        const auto first=run(argv[1],root,{});
        (void)run(argv[1],root,first);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
