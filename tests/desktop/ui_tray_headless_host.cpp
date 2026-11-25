#include "management/resident.hpp"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Json = nlohmann::json;

// 验收进程拥有独立窗口站和桌面，退出时归还全部句柄。
struct Desktop {
    HWINSTA station = nullptr;
    HDESK desktop = nullptr;
    // 先释放属于该站的桌面，再归还窗口站。
    ~Desktop() { if (desktop) CloseDesktop(desktop); if (station) CloseWindowStation(station); }
};
// 仅收回本次创建并持有内核句柄的后台，正式路径通过管道退出。
struct Child {
    PROCESS_INFORMATION info{};
    // 故障清理只终止本用例启动且仍在运行的子进程。
    ~Child() {
        if (info.hThread) CloseHandle(info.hThread);
        if (!info.hProcess) return;
        if (WaitForSingleObject(info.hProcess,0)==WAIT_TIMEOUT) {
            TerminateProcess(info.hProcess,2);
            WaitForSingleObject(info.hProcess,5000);
        }
        CloseHandle(info.hProcess);
    }
};

// 按真实命令行启动无任务栏的本体，再从本机管道核对业务存活与正常退出。
// 步骤：建立私有桌面 → 启动原发布程序 → 读取原实例状态并退出 → 验证图标登记结果。
int verify(const fs::path& app, const fs::path& root) {
    const auto name = L"DaFeiYuHeadless-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
    Desktop environment;
    environment.station = CreateWindowStationW(nullptr,0,WINSTA_ALL_ACCESS,nullptr);
    if (!environment.station && GetLastError()==ERROR_ACCESS_DENIED) return 77;
    const auto original = GetProcessWindowStation();
    USEROBJECTFLAGS flags{};
    wchar_t station_name[256]{};
    if (!environment.station || !GetUserObjectInformationW(environment.station,UOI_FLAGS,&flags,sizeof(flags),nullptr) ||
        (flags.dwFlags & WSF_VISIBLE) || !GetUserObjectInformationW(environment.station,UOI_NAME,station_name,sizeof(station_name),nullptr) ||
        !SetProcessWindowStation(environment.station)) throw std::runtime_error("could not enter noninteractive station");
    environment.desktop = CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if (!SetProcessWindowStation(original) || !environment.desktop) throw std::runtime_error("could not create private desktop");

    const auto work = root/name, config = work/"offline.json", data = work/"user data";
    fs::create_directories(data);
    std::ofstream(config) << R"({"provider":"deepseek","base_url":"https://offline.invalid","model":"fixture","api_key_env":"DFY_UNUSED_HEADLESS_FIXTURE"})";
    auto command_line = L"\""+app.wstring()+L"\" --host \""+config.wstring()+L"\" --data \""+data.wstring()+L"\"";
    auto desktop = std::wstring(station_name)+L"\\"+name;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = desktop.data();
    Child child;
    if (!CreateProcessW(app.c_str(),command_line.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child.info))
        throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"start fixture host");

    Json state;
    const auto until = std::chrono::steady_clock::now()+8s;
    do {
        try { state = dfy::management::resident_request(data,{{"op","status"}}); }
        catch (const std::exception&) { /* 等待这个子进程首次建立管道。 */ }
        if (state.is_object() && state.at("state")=="running") break;
        if (WaitForSingleObject(child.info.hProcess,50)!=WAIT_TIMEOUT) throw std::runtime_error("headless resident exited at startup");
    } while (std::chrono::steady_clock::now()<until);
    if (!state.is_object() || state.at("state")!="running" || state.at("pid")!=child.info.dwProcessId)
        throw std::runtime_error("did not observe the original headless host");
    dfy::management::request_stop(data,state.at("instance").get<std::string>(),"",nullptr);
    DWORD code = 1;
    if (WaitForSingleObject(child.info.hProcess,7000)!=WAIT_OBJECT_0 || !GetExitCodeProcess(child.info.hProcess,&code) || code)
        throw std::runtime_error("headless resident did not exit normally");

    for (const auto& file : fs::directory_iterator(data/"logs")) {
        if (file.path().extension()!=".jsonl") continue;
        std::ifstream input(file.path());
        for (std::string line; std::getline(input,line);) {
            const auto row = Json::parse(line);
            if (row.at("pid")==child.info.dwProcessId && row.at("event")=="tray.start" && row.at("result")=="failed") return 0;
        }
    }
    throw std::runtime_error("missing failed tray registration on the private desktop");
}
}

// 使用独立数据目录核对 GUI 开关两种构建共用的正式后台入口。
int wmain(int argc, wchar_t** argv) {
    try { return argc==3 ? verify(argv[1],argv[2]) : 2; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
