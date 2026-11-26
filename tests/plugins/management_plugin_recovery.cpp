#include "management/resident.hpp"
#include <dafeiyu/dynamic_library.hpp>
#include <dafeiyu/plugin_abi_v2.h>
#include <windows.h>
#include <sddl.h>
#include <atomic>
#include <fstream>
#include <iostream>

namespace {
using namespace dfy;
using namespace std::chrono_literals;
using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr auto first_id="3b75faab-3207-4b0a-ae38-e7a7c0e96ada";
constexpr auto second_id="41a882be-3187-4bc2-a720-9b7eaeaf9371";

// 将失败定位到请求投递、回执恢复或原生实例边界。
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }

// 归还本次原始管道夹具独占的文件或令牌句柄。
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    // 只关闭已成功取得的系统句柄。
    ~Handle() { if (value && value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
};

// 按公开传输所用的用户 SID 和物理目录身份连接真实端点，不从宿主取回命令结果。
std::wstring fixture_endpoint(const fs::path& profile) {
    Handle token, folder{CreateFileW(profile.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr)};
    BY_HANDLE_FILE_INFORMATION info{};
    check(GetFileInformationByHandle(folder.value,&info) && OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value),"fixture endpoint owner");
    DWORD size=0;
    GetTokenInformation(token.value,TokenUser,nullptr,0,&size);
    std::vector<unsigned char> user(size);
    check(GetTokenInformation(token.value,TokenUser,user.data(),size,&size),"fixture user SID");
    LPWSTR raw=nullptr;
    check(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid,&raw),"fixture SID text");
    std::unique_ptr<wchar_t,decltype(&LocalFree)> release(raw,&LocalFree);
    return L"\\\\.\\pipe\\DaFeiYu.Management.1."+std::wstring(raw)+L"."+std::to_wstring(info.dwVolumeSerialNumber)+
        L"."+std::to_wstring(info.nFileIndexHigh)+L"."+std::to_wstring(info.nFileIndexLow);
}

// 仅写入完整原始请求帧并等待服务端取走请求，直接关管道而不读回包或确认。
// 步骤：连接夹具进程的真实管道 → 写长度与正文 → 确认对端取走请求字节 → 丢弃连接。
void lose_first_reply(const fs::path& data,const std::string& client,Json body) {
    body.update(Json{{"version","1"},{"request","unread-first"}});
    const auto wire=Json{{"v",1},{"op","request"},{"client",client},{"body",std::move(body)}}.dump();
    const auto name=fixture_endpoint(data);
    check(WaitNamedPipeW(name.c_str(),5000),"fixture pipe did not reopen");
    Handle pipe{CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr)};
    ULONG pid=0;
    check(pipe.value!=INVALID_HANDLE_VALUE && GetNamedPipeServerProcessId(pipe.value,&pid) && pid==GetCurrentProcessId(),"wrong fixture pipe");
    const auto size=static_cast<DWORD>(wire.size());
    DWORD written=0;
    check(WriteFile(pipe.value,&size,sizeof(size),&written,nullptr) && written==sizeof(size) &&
        WriteFile(pipe.value,wire.data(),size,&written,nullptr) && written==size && FlushFileBuffers(pipe.value),"lost request before dispatch");
}

// 通过已握手的另一个管道连接正常读取管理响应。
Json call(const fs::path& data,const std::string& client,Json body) {
    body.update(Json{{"version","1"},{"request","probe"}});
    return management::resident_request(data,{{"op","request"},{"client",client},{"body",std::move(body)}});
}

// 有界等待原件变为可核对状态，查询期间绝不重发写入。
template<class Sample> void until(Sample sample,const char* reason) {
    const auto deadline=std::chrono::steady_clock::now()+15s;
    do {
        if (sample()) return;
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now()<deadline);
    throw std::runtime_error(reason);
}

// 验证首包无法取得回执时可以重连按事前编号查证，同时保护代次、命令水位及原提交内容。
// 步骤：准备真实 DLL 和两个窗口 → 丢弃首份加载回包 → 重连反复查原代次 → 验证冲突及卸载回执。
void recover(const fs::path& data,const fs::path& dll) {
    const auto packages=data/"packages", source=packages/"recover";
    fs::create_directories(source);
    fs::copy_file(dll,source/"entry.dll");
    std::ofstream(source/"plugin.json") << R"({"manifest_version":1,"id":"test.restore","name":"恢复","version":"1","abi_version":2,"entry":"entry.dll"})";
    platform::DynamicLibrary counters(source/"entry.dll");
    const auto count=reinterpret_cast<std::uint32_t (DFY_V2_CALL*)(std::uint32_t)>(counters.symbol("dfy_lifecycle_count"));
    std::atomic_uint calls{};
    int outcome=-1;
    std::jthread server([&](std::stop_token stop) {
        outcome=management::serve_resident(data,"loss",[&] {
            return std::make_unique<app::Application>(model::Config{},packages,data,model::Backend{
                [&](const auto&,const auto&) { ++calls; return model::Response{{model::Role::assistant,"fixed"}}; }});
        },stop);
    });
    until([&] {
        try { return management::resident_request(data,{{"op","status"}}).at("state")=="running"; }
        catch (const std::system_error&) { return false; }
    },"resident startup");
    const auto original=management::connect_resident(data,"loss"), observer=management::connect_resident(data,"loss");
    const Json list{{"type","plugins.list"}};
    const auto directory=call(data,observer.client,list);
    const auto& row=directory.at("packages").front();
    const Json load{{"type","plugins.load"},{"instance",original.instance},{"command","1"},{"revision",directory.at("revision")},
        {"id",row.at("id")},{"generation",row.at("generation")},{"submission",first_id}};
    const Json planned{{"type","plugins.receipt"},{"instance",original.instance},{"operation",std::string("plugin/request/")+first_id}};
    check(call(data,observer.client,planned).at("state")=="unknown" && !count(0),"looking up a preallocated identity started a load");
    lose_first_reply(data,original.client,load);
    auto replacement=load;
    replacement["submission"]=second_id;
    check(call(data,original.client,replacement).at("state")=="conflict","same window number accepted a new submission");
    (void)management::resident_request(data,{{"op","disconnect"},{"client",original.client}});
    const auto reopened=management::connect_resident(data,"loss");
    Json query{{"type","plugins.receipt"},{"instance",reopened.instance},{"operation",std::string("plugin/request/")+first_id}};
    Json loaded;
    until([&] {
        loaded=call(data,reopened.client,query);
        return loaded.at("state")=="succeeded";
    },"reopened client could not recover the unread first reply");
    check(loaded.at("operation")==query.at("operation") && loaded.at("submission")==first_id && loaded.at("generation")!="0" &&
        loaded.at("effect")=="loaded" && loaded.dump().find(original.client)==std::string::npos && count(0)==1 && count(1)==1,
        "public receipt leaked the author or started two instances");
    check(call(data,observer.client,query)==loaded && call(data,reopened.client,load)==loaded,"cross-window replay lost the original result");
    auto wrong=load;
    wrong["id"]="test.other";
    check(call(data,reopened.client,wrong).at("state")=="conflict","different target reused the public submission");
    const auto active=call(data,reopened.client,list);
    check(active.at("packages").front().at("generation")==loaded.at("generation") && count(0)==1,"receipt lookup re-executed load");
    Json unload{{"type","plugins.unload"},{"instance",reopened.instance},{"command","1"},{"revision",active.at("revision")},
        {"id",row.at("id")},{"generation",loaded.at("generation")},{"submission",second_id}};
    lose_first_reply(data,reopened.client,unload);
    query["operation"]=std::string("plugin/request/")+second_id;
    Json stopped;
    until([&] { stopped=call(data,observer.client,query); return stopped.at("state")=="succeeded"; },"unread initial unload was not recoverable");
    check(stopped.at("effect")=="unloaded" && stopped.at("generation")==loaded.at("generation") &&
        call(data,reopened.client,unload)==stopped && count(2)==1 && count(3)==1 && !calls,"recovering stop changed its native effects");

    auto obsolete=load;
    obsolete.erase("submission");
    obsolete["revision"]="0";
    for (unsigned index=1; index<=64; ++index) {
        obsolete["command"]=std::to_string(index);
        const auto accepted=call(data,observer.client,obsolete);
        check(accepted.at("state")=="queued","legacy command sequence changed after a public read");
        Json receipt{{"type","plugins.receipt"},{"instance",observer.instance},{"operation",accepted.at("operation")}};
        until([&] { return call(data,observer.client,receipt).at("state")=="stale"; },"legacy operation did not settle");
    }
    auto replay=load;
    replay["revision"]=call(data,observer.client,list).at("revision");
    const auto fresh=management::connect_resident(data,"loss");
    check(call(data,fresh.client,planned).at("state")=="expired" && call(data,fresh.client,replay).at("state")=="expired" &&
        call(data,fresh.client,query).at("state")=="expired" && count(0)==1,"retired public identity was accepted as new work");
    query["instance"]="previous-resident";
    check(call(data,fresh.client,query).at("state")=="stale" && !calls,"old instance resolved a current public operation");
    server.request_stop();
    server.join();
    check(!outcome,"resident failed on shutdown");
}
}

// 用独立用户目录和真实 ABI 夹具验证丢包恢复。
int wmain(int argc,wchar_t** argv) {
    try {
        if (argc!=3) return 2;
        const auto data=fs::path(argv[2])/std::to_string(GetCurrentProcessId())/std::to_string(GetTickCount64());
        recover(data,argv[1]);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
