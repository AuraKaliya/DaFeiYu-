#include "management/resident.hpp"
#include "management/protocol.hpp"
#include "fixtures/query_v2/mailbox_fixture.h"
#include <dafeiyu/dynamic_library.hpp>
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
void package(const fs::path& source, const fs::path& dll, const std::string& id="test.directory", const std::string& provider={}) {
    fs::create_directories(source);
    fs::copy_file(dll,source/"entry.dll");
    Json manifest{{"manifest_version",1},{"id",id},{"name","目录测试"},{"version","1"},{"abi_version",2},{"entry","entry.dll"}};
    if (!provider.empty()) manifest["dependencies"]={provider};
    std::ofstream(source/"plugin.json") << manifest.dump();
}

// 在释放夹具控制句柄前设置下一次原生停止的确定性结果。
void stop_mode(const fs::path& packages, unsigned mode) {
    platform::DynamicLibrary control(packages/"test.directory"/"entry.dll");
    const auto set=reinterpret_cast<void (DFY_V2_CALL*)(std::uint32_t)>(control.symbol("dfy_lifecycle_mode"));
    set(mode);
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

// 在已加载的提供方上验证停止失败保留、依赖拒绝与同一代次的显式重试。
// 步骤：注入一次真实停止失败 → 安装依赖包并核对缺少活动提供方 → 加载两方 → 从原回执核对依赖保护。
void dependency_results(const fs::path& root, const fs::path& dll) {
    const auto data=root/"user", packages=root/"packages", dependent=root/"dependent";
    const auto a=management::connect_resident(data,"offline"), b=management::connect_resident(data,"offline");
    const auto read=[&] { return call(data,b.client,{{"type","plugins.list"}}); };
    unsigned sequence=0;
    const auto perform=[&](const std::string& id,const char* action) {
        const auto view=read();
        for (const auto& row : view.at("packages")) if (row.at("id")==id)
            return settled(data,b.client,call(data,a.client,intent(view,row,action,++sequence)));
        throw std::runtime_error("dependency fixture package absent");
    };
    stop_mode(packages,3);
    const auto failed=perform("test.directory","unload");
    const auto retained=read().at("packages").front();
    check(failed.at("state")=="failed" && failed.at("nativeResult")=="failed" && failed.at("effect")=="retained" &&
        retained.at("state")=="failed" && retained.at("canUnload")==true && retained.at("generation")==failed.at("generation"),
        "failed native stop was not retained and retryable");
    check(perform("test.directory","unload").at("effect")=="unloaded" && settled(data,b.client,failed)==failed,
        "new stop did not complete independently of the original failure");
    package(dependent,dll,"test.dependent","test.directory");
    const auto path=dependent.generic_u8string();
    terminal(data,a.client,1,"/plugins install \""+std::string(path.begin(),path.end())+"\"");
    until([&] { return read().at("packages").size()==2; },"installed dependent");
    const auto missing=perform("test.dependent","load");
    check(missing.at("state")=="failed" && missing.at("effect")=="notLoaded" && missing.at("generation")=="0",
        "missing active dependency left the confirmed no-instance effect unknown");
    check(perform("test.directory","load").at("effect")=="loaded" && perform("test.dependent","load").at("effect")=="loaded",
        "declared dependencies could not load after their provider");
    const auto before=read();
    const auto protected_stop=perform("test.directory","unload");
    check(protected_stop.at("state")=="inUse" && protected_stop.at("nativeResult")=="inUse" && protected_stop.at("effect")=="retained" &&
        before==read(),"default unload removed an instance with a live dependent");
    check(perform("test.dependent","unload").at("effect")=="unloaded" && perform("test.directory","unload").at("effect")=="unloaded" &&
        settled(data,b.client,protected_stop)==protected_stop && settled(data,b.client,missing)==missing,
        "later cleanup changed an original dependency receipt");
    const auto damaged=root/"damaged";
    package(damaged,dll,"test.damaged");
    std::ofstream(damaged/"entry.dll",std::ios::binary|std::ios::trunc) << "invalid fixture module";
    const auto bad_path=damaged.generic_u8string();
    terminal(data,a.client,2,"/plugins install \""+std::string(bad_path.begin(),bad_path.end())+"\"");
    until([&] { return read().at("packages").size()==3; },"installed damaged fixture");
    const auto broken=perform("test.damaged","load");
    check(broken.at("state")=="failed" && broken.at("effect")=="notLoaded" && broken.at("generation")=="0" &&
        !broken.contains("nativeCode") && settled(data,b.client,broken)==broken,"native module opening failure fabricated a status or a running instance");
}

// 通过两个已握手的管道客户端核对磁盘包、独立运行代次和未被消费的到件。
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
    stop_mode(packages,6);
    const auto unload=intent(active,live,"unload",2);
    const auto accepted_stop=call(data,one.client,unload);
    check(accepted_stop.at("state")=="queued","unload was not admitted");
    const auto stopped=settled(data,two.client,accepted_stop);
    check(stopped.at("state")=="succeeded" && stopped.at("nativeResult")=="pending" && stopped.at("effect")=="unloaded" &&
        stopped.at("generation")==live.at("generation") && read(one.client).at("packages").front().at("state")=="installed",
        "stop acceptance was confused with actual exit");
    check(call(data,two.client,pinned).at("state")=="stale","old package revision was accepted");
    terminal(data,one.client,5,"/plugins load test.directory");
    until([&] { const auto current=read(two.client).at("packages").front();
        return current.at("state")=="active" && current.at("generation")!=live.at("generation"); },"replacement generation");
    auto old_target=unload;
    old_target["revision"]=read(two.client).at("revision");
    const auto old_stop=settled(data,one.client,call(data,two.client,old_target));
    check(old_stop.at("state")=="stale" && old_stop.at("effect")=="notApplied" && call(data,one.client,unload)==stopped,
        "old unload stopped the replacement or lost its original operation");
    const auto old=call(data,one.client,{{"type","plugins.list"},{"instance","previous-resident"}});
    check(old.at("state")=="stale" && old.at("packages").empty() && !model_calls,"old backend disclosed packages");
    dependency_results(root,dll);
    check(!model_calls && call(data,two.client,room)==pending,"dependency operations changed the passive mailbox or ran a model");
    server.request_stop();
    server.join();
    check(!outcome,"resident did not exit normally");
}

// 在插件自有子会话运行期间用两个正式管道请求卸载，只允许原工作退出后的新命令卸载。
// 步骤：真实 C DLL 提交固定工作 → 核对忙碌和原代次保留 → 断开原窗口并复查 → 释放工作后显式重试。
void busy_unload(const fs::path& root, const fs::path& dll) {
    const auto data=root/"busy-user", packages=root/"busy-packages";
    fs::create_directories(data);
    package(packages/"fixture",dll);
    std::atomic_bool release{false};
    std::atomic_uint calls{0};
    int outcome = -1;
    std::jthread server([&](std::stop_token stop) {
        // 仅这次原生子会话调用固定模型，夹具在后台停止时也能解开等待。
        outcome=management::serve_resident(data,"busy",[&] {
            auto app=std::make_unique<app::Application>(model::Config{},packages,data,model::Backend{
                [&,stop](const auto&,const auto&) {
                    ++calls;
                    until([&] { return release.load() || stop.stop_requested(); },"fixed model release");
                    return model::Response{{model::Role::assistant,"fixed"}};
                }});
            check(app->plugins().load("test.directory")==DFY_V2_OK,"busy plugin startup failed");
            const auto plugin=app->plugins().instance("test.directory");
            const auto api=plugin->query<dfy_test_mailbox_api>(DFY_TEST_MAILBOX_INTERFACE,1);
            std::uint64_t request=0;
            check(api && plugin->invoke(*api,[&](const auto& service,auto* target) { return service.ask(target,&request,nullptr); })==DFY_V2_OK &&
                request!=0,"native subagent work was not admitted");
            return app;
        },stop);
    });
    until([&] { return calls.load()!=0; },"busy native subagent");
    const auto a=management::connect_resident(data,"busy"), b=management::connect_resident(data,"busy");
    const Json list{{"type","plugins.list"}};
    const auto before=call(data,a.client,list);
    const auto unload=intent(before,before.at("packages").front(),"unload",1);
    const auto submitted=call(data,a.client,unload);
    const auto blocked=settled(data,b.client,submitted);
    check(blocked.at("state")=="busy" && blocked.at("effect")=="retained" && blocked.at("nativeResult")=="busy" &&
        call(data,b.client,list)==before && call(data,a.client,unload)==blocked && calls==1,"busy instance or original refusal did not survive");
    (void)management::resident_request(data,{{"op","disconnect"},{"client",a.client}});
    const auto reopened=management::connect_resident(data,"busy");
    check(settled(data,reopened.client,submitted)==blocked,"reconnection lost the original busy receipt");
    release=true;
    unsigned sequence=0;
    Json result;
    until([&] {
        const auto latest=call(data,b.client,list);
        auto retry=intent(latest,latest.at("packages").front(),"unload",++sequence);
        result=settled(data,b.client,call(data,b.client,retry));
        return result.at("state")!="busy" && result.at("state")!="inUse";
    },"explicit retry after native work exit");
    check(result.at("state")=="succeeded" && result.at("effect")=="unloaded" && result.at("generation")==blocked.at("generation") &&
        settled(data,reopened.client,submitted)==blocked && call(data,b.client,list).at("packages").front().at("loaded")==false &&
        calls==1,"unload retry changed the old refusal or unloaded a different generation");
    server.request_stop();
    server.join();
    check(!outcome,"busy resident did not exit normally");
}

// 用真正安装的独立测试包核对当前代次的公开字段、值和来源，其他配置留在实例内部。
// 步骤：双窗核对包默认与无声明包 → 改写用户文件并复读原代次 → 重载核对用户来源 → 隔离错误类型。
void public_settings(const fs::path& root,const fs::path& dll,const fs::path& fixture) {
    const auto data=root/"user", packages=root/"packages";
    fs::create_directories(data);
    package(packages/"ordinary",dll,"test.ordinary");
    std::atomic_uint calls{0};
    int outcome=-1;
    std::jthread server([&](std::stop_token stop) {
        outcome=management::serve_resident(data,"public-settings",[&] {
            auto app=std::make_unique<app::Application>(model::Config{},packages,data,model::Backend{
                [&](const auto&,const auto&) { ++calls; return model::Response{{model::Role::assistant,"fixed"}}; }});
            const auto installed=app->plugins().install(fixture);
            check(app->plugins().load(installed.id)==DFY_V2_OK && app->plugins().load("test.ordinary")==DFY_V2_OK,"settings fixture startup failed");
            return app;
        },stop);
    });
    until([&] {
        try { return management::resident_request(data,{{"op","status"}}).at("state")=="running"; }
        catch (const std::system_error&) { return false; }
    },"public settings startup");
    const auto a=management::connect_resident(data,"public-settings"), b=management::connect_resident(data,"public-settings");
    const auto catalog=[&] { return call(data,b.client,{{"type","plugins.list"}}); };
    const auto before=catalog();
    const auto plain=before.at("packages").at(0), target=before.at("packages").at(1);
    check(plain.at("hasPublicSettings")==false && target.at("hasPublicSettings")==true,"directory advertised the wrong setting owner");
    Json read{{"type","settings.read"},{"owner","plugin"},{"instance",a.instance},{"id",target.at("id")},{"generation",target.at("generation")}};
    const auto original=call(data,a.client,read);
    const Json defaults=Json::array({{{"id","displayName"},{"type","string"},{"access_mode","read_only"},{"source","default"},
        {"value","书房观察器"},{"appliedValue","书房观察器"}}});
    check(original.at("state")=="ready" && original.at("event")=="settings.snapshot" && original.at("revision")==read.at("generation") &&
        original.at("canSave")==false && original.at("fields")==defaults && original==call(data,b.client,read) &&
        original.dump().find("internal-fixture-only")==std::string::npos,"public default leaked a private setting or changed across windows");
    auto other=read;
    other["id"]=plain.at("id");
    check(call(data,b.client,other).at("state")=="stale","foreign generation accepted");
    other["generation"]=plain.at("generation");
    const auto none=call(data,b.client,other);
    check(none.at("state")=="notDeclared" && none.at("fields").empty(),"ordinary package fabricated a declaration");
    other=read;
    other["instance"]="old-resident";
    check(call(data,b.client,other).at("state")=="stale","wrong resident returned plugin settings");
    const auto file=data/"plugins"/"test.public-settings"/"settings.json";
    fs::create_directories(file.parent_path());
    std::ofstream(file) << R"({"displayName":"新观察器","privateOverride":"unlisted-user-marker"})";
    check(call(data,b.client,read)==original,"disk edit changed an already loaded generation");

    unsigned sequence=0;
    const auto reload=[&] {
        for (const auto action : {"unload","load"}) {
            const auto dir=catalog();
            const auto requested=intent(dir,dir.at("packages").at(1),action,++sequence);
            check(settled(data,b.client,call(data,a.client,requested)).at("state")=="succeeded","public settings reload failed");
            if (std::string_view(action)=="unload") check(catalog().at("packages").at(1).at("hasPublicSettings")==false,
                "unloaded package still advertised a public instance setting");
        }
    };
    reload();
    const auto expired=call(data,b.client,read);
    check(expired.at("state")=="stale" && expired.at("fields").empty(),"retired generation disclosed settings");
    read["generation"]=catalog().at("packages").at(1).at("generation");
    const auto updated=call(data,b.client,read);
    check(updated.at("fields").size()==1 && updated.at("fields").front().at("value")=="新观察器" &&
        updated.at("fields").front().at("source")=="user" && updated.dump().find("privateOverride")==std::string::npos &&
        updated.dump().find("unlisted-user-marker")==std::string::npos,"user override or allowlist was lost");
    read["revision"]=original.at("revision");
    check(call(data,a.client,read).at("state")=="stale","old settings revision matched a replacement");
    read.erase("revision");
    std::ofstream(file,std::ios::trunc) << R"({"displayName":{"nested":"private-object"}})";
    reload();
    read["generation"]=catalog().at("packages").at(1).at("generation");
    const auto bad=call(data,a.client,read);
    check(bad.at("state")=="unavailable" && bad.at("fields").empty() && catalog().at("packages").at(1).at("state")=="active" && !calls,
        "invalid public type leaked nested data, stopped the plugin or ran a model");
    server.request_stop();
    server.join();
    check(!outcome,"public settings resident did not exit normally");
}

// 固定宿主推进节拍，核对 DLL 已关闭但原代次尚有一项调度包装时的目录和原回执。
// 步骤：加载三个实际活动插件 → 在第三方仍排队时停止 → 冻结现场核对原代次 → 推进排空并复查。
void queued_activity(const fs::path& root,const fs::path& dll) {
    const auto data=root/"activity-user", packages=root/"activity-packages";
    fs::create_directories(data);
    for (const std::string id : {"activity.a","activity.b","activity.c"}) package(packages/id,dll,id);
    std::atomic_uint permit{1},entered{0},model_calls{0};
    std::atomic_bool release{false};
    management::Host host([&] {
        auto app=std::make_unique<app::Application>(model::Config{},packages,data,model::Backend{
            [&](const auto&,const auto&) { ++model_calls; return model::Response{{model::Role::assistant,"fixed"}}; }});
        for (const auto id : {"activity.a","activity.b","activity.c"}) check(app->plugins().load(id)==DFY_V2_OK,"activity fixture failed to load");
        return app;
    },[&](auto&) {
        const auto current=++entered;
        until([&] { return release.load() || permit.load()>=current; },"activity fixture tick gate");
    },nullptr,{},"activity-host");
    struct Unlock {
        std::atomic_bool& flag;
        // 断言失败也先放行业务线程，保证宿主能够正常收尾。
        ~Unlock() { flag=true; }
    } unlock{release};
    const auto a=host.open_client(), b=host.open_client();
    const auto request=[&](const std::string& client, Json input) {
        input.update(Json{{"version","1"},{"request","frozen"}});
        return management::dispatch(&host,input,client);
    };
    until([&] { return entered>=2; },"initial activity sample");
    const Json list{{"type","plugins.list"}};
    const auto original=request(a,list), target=original.at("packages").at(2);
    check(target.at("id")=="activity.c" && target.at("state")=="active","third activity was not running");
    const auto accepted=request(a,intent(original,target,"unload",1));
    const Json lookup{{"type","plugins.receipt"},{"instance",original.at("instance")},{"operation",accepted.at("operation")}};
    permit=2;
    until([&] { return entered>=3; },"stop before queued activity drains");
    const auto interim=request(b,lookup), retained=request(b,list).at("packages").at(2);
    check(interim.at("nativeResult")=="closed" && retained.at("state")=="stopping" && retained.at("generation")==target.at("generation"),
        "fixture did not retain the original scheduling owner after native close");
    check(interim.at("state")=="pending" && interim.at("effect")=="stopping","native close claimed exit before the retained owner drained");
    permit=3;
    until([&] { return entered>=4; },"activity wrapper exit");
    const auto complete=request(b,lookup);
    check(complete.at("state")=="succeeded" && complete.at("nativeResult")=="closed" && complete.at("effect")=="unloaded" &&
        request(a,list).at("packages").at(2).at("generation")=="0" && !model_calls,"drained owner did not settle the original stop");
}
}

// 为每次运行隔离插件包、命名管道和用户设置，以固定模型验证管理入口。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc!=4 && argc!=5) return 2;
        const auto root=fs::path(argv[2])/std::to_string(GetCurrentProcessId())/std::to_string(GetTickCount64());
        if (argc==5) {
            if (std::wstring_view(argv[3])!=L"--public-settings") return 2;
            public_settings(root,argv[1],argv[4]);
            return 0;
        }
        directory(root,argv[1]);
        busy_unload(root,argv[3]);
        queued_activity(root,argv[3]);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
