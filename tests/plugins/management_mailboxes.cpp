#include "management/resident.hpp"
#include "mailbox_view.hpp"
#include "management/protocol.hpp"
#include "management/client.hpp"
#include "fixtures/query_v2/mailbox_fixture.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <atomic>
#include <algorithm>
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

// 在测试包原位写入指定的清单显示名，用于区分磁盘版本和实际运行代次。
void write_manifest(const fs::path& dir, std::string_view name) {
    std::ofstream(dir/"plugin.json") << Json{{"manifest_version",1},{"id","test.mail"},{"name",name},
        {"version","1"},{"abi_version",2},{"entry","entry.dll"},{"default_config","defaults.json"}}.dump();
}

// 建立可通过正式插件命令加载的 C DLL 包。
void package(const fs::path& dir, const fs::path& dll) {
    fs::create_directories(dir);
    fs::copy_file(dll,dir/"entry.dll");
    write_manifest(dir,"测试信箱");
    std::ofstream(dir/"defaults.json") << R"({"mode":"t"})";
}

// 在业务线程加载真实测试 DLL，并向其原地址投递一封能由夹具消费的请求。
// 步骤：确认加载成功 → 取得该代次的入站地址 → 从已绑定的本体投递。
void prepare_request(app::Application& app) {
    check(app.plugins().load("test.mail")==DFY_V2_OK,"receipt plugin failed to load");
    const auto address = app.plugins().instance("test.mail")->mailbox_addresses().inbound;
    const auto& mail = app.mailbox_host();
    mail.network->deliver_next(mail.identity,{address},events::mailbox_text("test.request",1,"source",32));
}

// 让原插件通过 C ABI 取走请求并按原序号发回一封信。
void reply_to_request(plugins::PluginInstance& plugin) {
    const auto api = plugin.query<dfy_test_mailbox_api>(DFY_TEST_MAILBOX_INTERFACE,1);
    check(api && plugin.invoke(*api,[](const auto& service,auto* target) { return service.reply(target,nullptr); })==DFY_V2_OK,
        "original DLL failed to receive and reply");
}

// 在原数据目录重启后台，由真实 DLL 在窗口连接前回信，两个管道客户端均从同一原件查回执。
// 步骤：建立待收回信 → 用旧实例查内置和插件来源 → 双客户端复查 → 单窗断开再连。
void restart_with_mail(const fs::path& data, const fs::path& packages, const std::string& previous, const std::string& old_source) {
    std::atomic_uint replies{0}, model_calls{0};
    int outcome = -1;
    // 在同目录再次持有正式服务端，退出时归还管道和数据库。
    std::jthread server([&](std::stop_token stop) {
        // 在业务线程准备真实收发，观察端只能等待后台发布完成的样本。
        // 步骤：加载有信箱的插件 → 投递夹具请求 → 通过 C ABI 生成一封待收回信。
        outcome = management::serve_resident(data,"offline",[&] {
            auto app = std::make_unique<app::Application>(model::Config{},packages,data,model::Backend{
                [&](const auto&,const auto&) { ++model_calls; return model::Response{{model::Role::assistant,"fixed"}}; }});
            prepare_request(*app);
            const auto plugin = app->plugins().instance("test.mail");
            reply_to_request(*plugin);
            const auto& mail = app->mailbox_host();
            check(mail.network->front(mail.identity,{plugin->mailbox_addresses().outbound}),"DLL did not leave a pending reply");
            ++replies;
            return app;
        },stop);
    });
    until([&] {
        try { return management::resident_request(data,{{"op","status"}}).at("state")=="running"; }
        catch (const std::system_error&) { return false; }
    },"replacement resident startup");
    const auto first = management::connect_resident(data,"offline"), second = management::connect_resident(data,"offline");
    const auto call = [&](const std::string& client, Json body) {
        body.update(Json{{"version","1"},{"request","reopen"}});
        return management::resident_request(data,{{"op","request"},{"client",client},{"body",std::move(body)}});
    };
    const auto listing = call(first.client,{{"type","mailbox.list"}});
    check(first.instance!=previous && listing.at("instance")==first.instance,"restarted resident reused its previous instance");
    for (const auto& source : {old_source,std::string("builtin/room")}) {
        const auto stale = call(second.client,{{"type","mailbox.read"},{"instance",previous},{"source",source}});
        check(stale.at("state")=="stale" && stale.at("messages").empty() && stale.at("mailboxes").empty(),"old resident disclosed new mail");
    }
    const Json snapshot{{"type","mailbox.read"},{"instance",first.instance},{"source",listing.at("sources").back().at("id")}};
    const auto before = call(first.client,snapshot);
    const auto& answer = before.at("messages").at(0);
    check(before.at("messages").size()==2 && answer.at("category")=="test.reply" && answer.at("state")=="pending" &&
        before.at("messages").at(1).at("state")=="received","named pipe did not expose the pending DLL reply");
    const auto address = answer.at("receipt").at("mailbox");
    const auto found = std::ranges::find_if(before.at("mailboxes"),[&](const auto& box) { return box.at("id")==address; });
    check(found!=before.at("mailboxes").end() && found->at("accepted")=="1" && found->at("received")=="0","wrong original consumer cursor");

    auto detail = snapshot;
    detail.update(Json{{"message",answer.at("id")},{"revision",before.at("revision")}});
    const auto original = call(second.client,detail);
    check(original.at("messages")==Json::array({answer}) && call(first.client,detail)==original,"two pipe clients saw different receipts");
    check(management::resident_request(data,{{"op","disconnect"},{"client",first.client}}).at("state")=="disconnected","observer failed to disconnect");
    const auto reopened = management::connect_resident(data,"offline");
    check(call(second.client,snapshot)==before && call(reopened.client,detail)==original && replies==1 && model_calls==0,
        "opening, disconnecting or reconnecting a viewer consumed or duplicated the DLL reply");
    server.request_stop();
    server.join();
    check(outcome==0,"replacement resident failed to drain");
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
    check(original.at("label")=="测试信箱","source label did not come from its loaded manifest");
    auto pinned_list=list;
    pinned_list.update(Json{{"instance",first.instance},{"revision",loaded.at("revision")}});
    check(call(second.client,pinned_list)==loaded,"matching directory revision was unavailable");
    const Json read{{"type","mailbox.read"},{"instance",first.instance},{"source",original.at("id")}};
    const auto empty=call(first.client,read);
    check(empty.at("event")=="mailbox.snapshot" && empty.at("state")=="ready" && empty.at("messages").empty() &&
        empty.at("mailboxes").size()==2 && empty==call(second.client,read),"native empty source was unreadable");
    {
        management::Client window(data,"offline",[] { throw std::runtime_error("unexpected replacement resident"); });
        auto invalid=read;
        invalid.update(Json{{"version","1"},{"request","malformed"},{"revision",false}});
        Json error;
        check(window.post(invalid,[&](const Json& value) { error=value; }),"native client did not admit a query");
        until([&] { window.advance(); return !error.is_null(); },"management error reply");
        check(error.at("event")=="management.error" && error.at("request")=="malformed","malformed read appeared as an empty source");
    }
    auto wrong=read;
    wrong["instance"]="previous-backend";
    check(call(first.client,wrong).at("state")=="stale","source escaped its original resident");
    check(original.at("kind")=="plugin" && original.at("plugin")=="test.mail" && original.at("generation").is_string() &&
        loaded.at("revision")!=initial.at("revision") && loaded==call(second.client,list),"plugin not bound to its original generation");
    write_manifest(packages/"mail","新一代信箱");
    command("/plugins refresh");
    check(call(second.client,list)==loaded,"catalog refresh relabeled a live generation");
    command("/plugins unload test.mail");
    until([&] { return call(first.client,list).at("sources").size()==2; },"directory after unload");
    check(call(second.client,read).at("state")=="stale" && call(first.client,pinned_list).at("state")=="stale",
        "unloaded source or old directory remained readable");
    command("/plugins load test.mail");
    Json reloaded;
    until([&] { reloaded=call(second.client,list); return reloaded.at("sources").size()==3; },"directory after reload");
    const auto replacement=reloaded.at("sources").back();
    check(replacement.at("label")=="新一代信箱" && replacement.at("plugin")==original.at("plugin"),
        "reloaded source did not acquire its new manifest label");
    auto fresh=read;
    fresh["source"]=replacement.at("id");
    check(call(first.client,read).at("messages").empty() && call(first.client,read).at("state")=="stale" &&
        call(second.client,fresh).at("state")=="ready","new generation exposed the previous snapshot");
    check(replacement.at("id")!=original.at("id") && replacement.at("generation")!=original.at("generation") &&
        reloaded.at("revision")!=loaded.at("revision") && model_calls==0,"reloaded source reused its identity or viewing started a turn");
    server.request_stop();
    server.join();
    check(outcome==0,"resident failed to drain");
    restart_with_mail(data,packages,first.instance,original.at("id").get<std::string>());
}

// 以真实插件事件触发懒登记，验证业务信箱仍属于原代次，反复取样保留原队首。
// 步骤：取得仅有双向地址的基线 → 从 C DLL 发出已登记的业务事件 → 核对新增地址与实件 → 重采并检查原消费水位。
void routed_source(const fs::path& root) {
    std::atomic_uint model_calls{0};
    app::Application app({},root/"plugins",root/"routed",model::Backend{
        [&](const auto&,const auto&) { ++model_calls; return model::Response{{model::Role::assistant,"fixed"}}; }});
    app.wake_for_plugin_mail(false);
    check(app.plugins().load("test.mail")==DFY_V2_OK,"routed plugin failed to load");
    const auto plugin = app.plugins().instance("test.mail");
    const auto direct = plugin->mailbox_addresses();
    const auto before = app::sample_mailboxes(app,{});
    const auto registered = std::ranges::find_if(before->directory->sources,[](const auto& row) { return row.second.plugin=="test.mail"; });
    check(registered!=before->directory->sources.end() && registered->second.bindings.size()==2,"missing initial mailbox pair");
    const auto id = registered->first;
    const auto api = plugin->query<dfy_test_mailbox_api>(DFY_TEST_MAILBOX_INTERFACE,1);
    check(api && plugin->invoke(*api,[](const auto& service,auto* target) { return service.emit_notice(target,1,0); })==DFY_V2_OK,
        "registered C notice was not admitted");

    auto current = before;
    until([&] {
        app.advance_plugins();
        current = app::sample_mailboxes(app,current);
        return current->directory->sources.at(id).bindings.size()==3;
    },"registered route publication");
    const auto& bindings = current->directory->sources.at(id).bindings;
    const auto extra = std::ranges::find_if(bindings,[&](const auto& binding) {
        return binding.mailbox.value!=direct.inbound && binding.mailbox.value!=direct.outbound;
    });
    const auto& mail = app.mailbox_host();
    check(extra!=bindings.end() && extra->sender==plugin->generation() && extra->receiver==mail.identity &&
        current->directory->sources.size()==before->directory->sources.size() && current->directory->revision!=before->directory->revision &&
        current->sources.at("builtin/room")==before->sources.at("builtin/room"),"business route split the source or revised an unrelated source");
    const auto index = static_cast<std::size_t>(extra-bindings.begin());
    const auto& route = current->sources.at(id)->addresses.at(index);
    check(route.accepted==1 && !route.received && route.messages.size()==1 && route.messages.front().category=="test.notice",
        "routed notice lost its original category or receipt");
    for (int view=0; view<3; ++view) {
        app.advance_plugins();
        check(app::sample_mailboxes(app,current)==current,"viewing a passive notice changed its published revision");
    }
    const auto* original = mail.network->front(mail.identity,extra->mailbox);
    check(original && original->id==route.messages.front().event && !model_calls &&
        !mail.network->front(mail.identity,{direct.outbound}),"view consumed the real business notice, generated a reply or started the model");
}

// 让原系统持有收件和回信权，用真实 C DLL 在两端之间交付一封待处理的回信。
// 步骤：投递原件并反复查看 → 由 DLL 实收并回信 → 换窗口查原号 → 本体实际收件后再核对。
void receipts(const fs::path& root) {
    std::atomic_uint model_calls{0}, replies{0}, reads{0}, step{0};
    // 为受控业务线程建立真实插件和待处理原件。
    // 步骤：创建应用并加载 DLL → 向其登记地址实际投递 → 启动原有推进循环。
    management::Host host([&] {
        auto app = std::make_unique<app::Application>(model::Config{},root/"plugins",root/"receipts",model::Backend{
            [&](const auto&,const auto&) { ++model_calls; return model::Response{{model::Role::assistant,"fixed"}}; }});
        prepare_request(*app);
        write_manifest(root/"plugins"/"mail","尚未加载的名称");
        app->plugins().refresh();
        const auto latest = app->plugins().catalog();
        const auto view = app->mailbox_directory();
        const auto active = std::ranges::find_if(view.sources,[](const auto& row) { return row.second.plugin=="test.mail"; });
        check(latest->packages.at("test.mail").name=="尚未加载的名称" && active!=view.sources.end() && active->second.label=="新一代信箱",
            "a fresh business sample borrowed the replacement manifest name");
        app->post_room_event(app::RoomEvent::presence_changed,"书房","private-room-mailbox-marker");
        return app;
    },
    // 每个指令只交给原接收方一次，先从真实队首核对之前查看的影响。
    // 步骤：核对实际队首 → 调用插件回信或本体取件 → 记录完成次数。
    [&](app::Application& app) {
        const auto action = step.exchange(0);
        if (!action) return;
        const auto plugin = app.plugins().instance("test.mail");
        const auto [inbound,outbound] = plugin->mailbox_addresses();
        const auto& mail = app.mailbox_host();
        if (action==1) {
            const auto* pending = mail.network->front(plugin->generation(),{inbound});
            check(pending && pending->id.sequence==1 && !mail.network->front(mail.identity,{outbound}),"view consumed mail or generated a reply");
            reply_to_request(*plugin);
            ++replies;
        } else {
            check(!mail.network->front(plugin->generation(),{inbound}),"DLL left its consumed request queued");
            const auto result = mail.network->read(mail.identity,{outbound});
            check(result.event && result.event.get().id.sequence==1 &&
                result.event.get().payload->find("text")->as_string()=="received" && !mail.network->front(mail.identity,{outbound}),
                "observer consumed or duplicated the native reply");
            ++reads;
        }
    },nullptr,{},"receipt-host");
    until([&] { return bool(host.dialogue().display); },"receipt host startup");
    const auto first = host.open_client(), second = host.open_client();
    const auto query = [&](const std::string& client, Json body) {
        body.update(Json{{"version","1"},{"request","observe"},{"instance",host.instance()}});
        return management::dispatch(&host,body,client);
    };
    const auto directory = query(first,{{"type","mailbox.list"}});
    const Json box{{"type","mailbox.read"},{"source",directory.at("sources").back().at("id")}};
    const Json room{{"type","mailbox.read"},{"source","builtin/room"}};
    const auto initial_room = query(first,room);
    check(initial_room.at("messages").at(0).at("category")=="room.presence" &&
        initial_room.dump().find("private-room-mailbox-marker")==std::string::npos,"private mailbox body reached management");
    const auto pending = query(first,box);
    const auto& request = pending.at("messages").at(0);
    auto wrong_source = room;
    wrong_source["message"] = request.at("id");
    const auto missing = query(first,wrong_source);
    check(missing.at("state")=="stale" && missing.at("messages").empty() && missing.at("mailboxes").empty(),
        "an existing message was exposed in an unrelated source");
    auto detail = box;
    detail.update(Json{{"message",request.at("id")},{"revision",pending.at("revision")}});
    check(request.at("category")=="test.request" && request.at("direction")=="toPlugin" && request.at("state")=="pending" &&
        request.size()==6 && request.at("receipt").size()==5 && request.at("receipt").at("state")=="accepted" &&
        !request.at("receipt").contains("receivedUtcMs") &&
        pending==query(second,box) && pending==query(first,detail) && !model_calls && !replies && !reads,"view changed pending request");

    step = 1;
    Json answered;
    until([&] { answered=query(second,box); return answered.at("messages").size()==2; },"native DLL reply");
    const auto& answer = answered.at("messages").at(0);
    const auto& taken = answered.at("messages").at(1);
    check(taken.at("id")==request.at("id") && taken.at("state")=="received" && taken.at("receipt").contains("receivedUtcMs") &&
        taken.at("receipt").at("acceptedRevision")==request.at("receipt").at("acceptedRevision") &&
        answer.at("category")=="test.reply" && answer.at("direction")=="toHost" && answer.at("state")=="pending" &&
        answer.size()==6 && answered.at("revision")!=pending.at("revision") && query(first,detail).at("state")=="stale" &&
        query(second,room)==initial_room && directory==query(first,{{"type","mailbox.list"}}),"public receipts or revision isolation failed");
    detail["message"] = answer.at("id");
    detail["revision"] = answered.at("revision");
    host.close_client(first);
    const auto reconnected = host.open_client();
    check(query(reconnected,detail).at("messages")==Json::array({answer}) && answered==query(second,box) &&
        replies==1 && !reads && !model_calls,"viewing generated, consumed or restarted work");
    step = 2;
    Json completed;
    until([&] { completed=query(second,box); return completed.at("messages").at(0).at("state")=="received"; },"native host read");
    check(completed.at("messages").at(0).at("id")==answer.at("id") && reads==1 && replies==1 && !model_calls,
        "actual receipt did not stay attached to the original reply");
    host.close();
    until([&] { return host.exited(); },"receipt host shutdown");
    check(!host.failed(),"receipt host failed to drain");
}
}

// 在每次运行专用的目录中验证管理信箱，不使用真实模型。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc!=3) return 2;
        const auto root=fs::path(argv[2])/std::to_string(GetCurrentProcessId())/std::to_string(GetTickCount64());
        directory(root,argv[1]);
        receipts(root);
        routed_source(root);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
