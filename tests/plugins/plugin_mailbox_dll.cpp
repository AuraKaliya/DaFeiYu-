#include <dafeiyu/plugin_manager.hpp>
#include "fixtures/query_v2/mailbox_fixture.h"
#include "application.hpp"
#include "management/terminal_host.hpp"
#include "message_store.hpp"
#include <fstream>
#include <iostream>
#include <atomic>
#include <thread>
#include <nlohmann/json.hpp>

namespace {
using namespace dfy::events;
using namespace dfy::plugins;
using namespace dfy::runtime;
namespace fs = std::filesystem;

// 报告跨 DLL 读写和实例销毁的具体失败点。
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

// 构建本次独立测试包，保留其他运行的目录。
fs::path package(const fs::path& root, const fs::path& dll) {
    fs::create_directories(root);
    for (unsigned index = 1;; ++index) {
        const auto path = root/std::to_string(index)/"plugins"/"mail";
        if (!fs::create_directories(path)) continue;
        fs::copy_file(dll,path/"entry.dll");
        std::ofstream manifest(path/"plugin.json");
        manifest << R"({"manifest_version":1,"id":"test.mail","name":"mailbox example","version":"1","abi_version":2,"entry":"entry.dll"})";
        return path.parent_path();
    }
}

// 以同一 C DLL 构建独立的实例，依赖声明及默认配置在包内固定。
void converter(const fs::path& root, const fs::path& dll, std::string_view name, char mode, std::string_view provider = {}) {
    const auto path = root/name;
    fs::create_directories(path);
    fs::copy_file(dll,path/"entry.dll");
    std::ofstream defaults(path/"defaults.json");
    defaults << "{\"mode\":\"" << mode << "\"}";
    std::ofstream manifest(path/"plugin.json");
    manifest << R"({"manifest_version":1,"id":")" << name <<
        R"(","name":"converter","version":"1","abi_version":2,"entry":"entry.dll","default_config":"defaults.json")";
    if (!provider.empty()) manifest << R"(,"dependencies":[")" << provider << R"("])";
    manifest << '}';
}

// 在原加载顺序内验证独立、接续及非法返回的字段改写。
// 步骤：加载提供方及多个改写者 → 校验冲突和作用域 → 替换成依赖接续 → 检查非法长度。
void text_hooks(const fs::path& root, const fs::path& dll) {
    const auto packages = root/"converters";
    converter(packages,dll,"test.a",'a');
    converter(packages,dll,"test.b",'b',"test.a");
    converter(packages,dll,"test.c",'c');
    converter(packages,dll,"test.d",'d');
    converter(packages,dll,"test.z",'z');
    PluginMailboxHost host{std::make_shared<Mailboxes>(MailboxLimits{10,2,20,64}),next_domain()};
    auto channel = std::make_shared<BroadcastChannel>(std::make_shared<EventStore>(8,64),2,2);
    PluginManager manager(packages,root/"converter-data",channel,5,host);
    dfy_v2_text_hook_input event{DFY_V2_TEXT_HOOK_INPUT,0,1,1,0,0,0,1,{"probe",5},{"?",1}};
    check(!manager.load("test.a") && !manager.load("test.d") && manager.rewrite_hook(event,8) == "a","equal independent changes failed");
    check(!manager.load("test.c"),"conflict DLL failed to load");
    bool conflict = false;
    try { (void)manager.rewrite_hook(event,8); }
    catch (const std::runtime_error& issue) { conflict = std::string_view(issue.what()).find("conflicting proposals") != std::string_view::npos; }
    check(conflict,"independent field conflict was silently ordered");
    event.owner = next_domain();
    check(!manager.rewrite_hook(event,8),"field Hook exceeded its session scope");
    event.owner = 0;
    check(manager.unload("test.c") == ShutdownResult::closed && manager.unload("test.d") == ShutdownResult::closed &&
        !manager.load("test.b") && manager.rewrite_hook(event,8) == "b","declared dependency lost preceding change");
    check(!manager.load("test.z"),"invalid result DLL failed to load");
    bool bounded = false;
    try { (void)manager.rewrite_hook(event,8); } catch (const std::length_error&) { bounded = true; }
    check(bounded && manager.close(),"oversized field result or cleanup was not enforced");
}

// 通过正式应用验证 Hook 可以拒绝主回合并给原生工具加审批。
// 步骤：装配固定后端与原生包 → 批准原候选并核对实效 → 拒绝新请求 → 卸载后恢复普通问答。
void main_hook(const fs::path& root, const fs::path& status_package) {
    using namespace dfy::model;
    std::atomic<unsigned> calls{0};
    dfy::app::Application app({},root,root.parent_path()/"app-data",Backend{
        [&](const auto&,const Request& request) {
            ++calls;
            if (request.messages.back().content == "read mode")
                return Response{{Role::assistant,"",{{"read",request.tools.at(0).name,"{}"}}},FinishReason::tool_calls};
            return Response{{Role::assistant,request.messages.back().role == Role::tool ? request.messages.back().content : "allowed"}};
        }});
    app.plugins().install(status_package);
    check(!app.plugins().load("test.mail") && !app.plugins().load("example.status"),"Hook application setup failed");
    // 推进原会话直到等待审批或报告结算并实际退出。
    const auto advance = [&](bool approval) {
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
        std::optional<dfy::app::CompletedTurn> result;
        while (std::chrono::steady_clock::now() < deadline) {
            if (approval && !app.confirmations().empty()) return dfy::app::CompletedTurn{};
            if (auto polled = app.conversation().advance()) result = std::move(polled);
            if (result && !app.conversation().busy()) return *result;
            std::this_thread::yield();
        }
        throw std::runtime_error("Hook application did not reach expected boundary");
    };
    app.submit_read_only("example.status/read","read mode");
    advance(true);
    const auto waiting = app.confirmations();
    check(waiting.size() == 1 && waiting[0].state == dfy::agent::ConfirmationState::waiting && calls == 1,"tool Hook did not wait for approval");
    app.decide(waiting[0].id,waiting[0].revision,true);
    check(advance(false).answer.find("idle") != std::string::npos && calls == 2,"approved Hook candidate did not execute original tool");
    bool blocked = false;
    try { app.conversation().submit("blocked"); }
    catch (const std::runtime_error& error) { blocked = std::string_view(error.what()) == "run: model request denied"; }
    check(blocked && calls == 2,"deny Hook did not prevent model dispatch");
    check(app.plugins().unload("test.mail") == ShutdownResult::closed,"control Hook held an idle plugin");
    app.conversation().submit("hello");
    check(advance(false).answer == "allowed" && calls == 3,"unloaded Hook continued to deny");
    app.conversation().close();
    check(app.close_plugins() == ShutdownResult::closed,"Hook application did not release resources");
}

// 在正式主会话核对改写、内部结算和已送达的有效文本均保留各自身份。
// 步骤：载入转换插件与业务工具 → 核对改写参数的批准和退出 → 检查回复投递与历史边界。
void run_text_hooks(const fs::path& root, const fs::path& dll, const fs::path& status_package) {
    using namespace dfy::app;
    using namespace dfy::model;
    const auto packages = root/"text-hook-app";
    converter(packages,dll,"test.e",'e');
    std::string delivered_key;
    std::atomic<unsigned> calls{0};
    Application app({},packages,root/"text-hook-data",Backend{[&](const auto&,const Request& input) {
        if (++calls == 1) {
            check(input.messages.at(0).content == "hook system" && input.messages.back().content == "rewritten prompt",
                "original request did not pass through both text Hooks");
            return Response{{Role::assistant,"",{{"read",input.tools.at(0).name,R"({"bad":1})"}}},FinishReason::tool_calls};
        }
        if (calls == 2) {
            check(input.messages.back().role == Role::tool && input.messages.back().content.find("idle") != std::string::npos,
                "transformed tool did not deliver its original effect");
            return Response{{Role::assistant,"raw"}};
        }
        if (calls == 3) check(input.messages.at(input.messages.size()-2).content == "model text","internal output lost its original history");
        if (calls == 4) return Response{{Role::assistant,"raw"}};
        if (calls == 5) {
            const auto& status = input.messages.at(input.messages.size()-2).content;
            check(status.find("visible reply") != status.npos && status.find(delivered_key) != status.npos,
                "next turn did not see confirmed delivery text and original ID");
        }
        return Response{{Role::assistant,"done"}};
    }});
    auto channel = std::make_shared<MessageStore>(root/"text-hook-data");
    app.bind_messages(channel,"text-hook-host",nullptr);
    app.plugins().install(status_package);
    check(!app.plugins().load("test.e") && !app.plugins().load("example.status"),"could not start text Hook fixture");
    // 推进原 Agent 到业务结算和实际退出，保留导致失败的精确异常。
    const auto finish = [&]() {
        std::optional<CompletedTurn> result;
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while (app.conversation().busy() && std::chrono::steady_clock::now() < deadline) {
            if (auto report = app.conversation().advance()) result = std::move(report);
            std::this_thread::yield();
        }
        check(result.has_value() && !app.conversation().busy(),"text Hook turn did not exit");
        if (result->error) std::rethrow_exception(result->error);
        return result->answer;
    };
    app.submit_read_only("example.status/read","hook input");
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while (app.conversation().busy() && app.confirmations().empty() && std::chrono::steady_clock::now() < deadline) {
        app.conversation().advance();
        std::this_thread::yield();
    }
    const auto tickets = app.confirmations();
    check(tickets.size() == 1 && tickets[0].target.arguments == "{}" && calls == 1,"approval did not bind transformed arguments once");
    app.decide(tickets[0].id,tickets[0].revision,true);
    check(finish() == "visible reply","model response and reply delivery did not apply in sequence");
    check(app.plugins().unload("test.e") == ShutdownResult::closed,"idle Hook fixture did not unload");
    app.submit_dialogue("another turn");
    check(finish() == "done" && calls == 3,"unload changed conversation history or retained Hook");
    check(!app.plugins().load("test.e"),"could not reattach delivery hook");
    app.submit_dialogue("speak to user");
    check(finish() == "visible reply","channel-facing answer missed final hook");
    const auto rows = channel->page(true,0,channel->watermark(),1);
    check(rows.records.size() == 1 && rows.records[0].envelope.at("text") == "visible reply" &&
        rows.records[0].receipt.at("state") == "delivered","channel did not commit the effective text");
    delivered_key = rows.records[0].submission;
    app.submit_dialogue("what did you just say");
    check(finish() == "done" && calls == 5,"reading a confirmed output repeated outside work");
}

// 固定玩家世界内验证幂等、断序及十次插话后的原工作；换世界只允许原工作结算。
// 步骤：提交基线及工作 → 增量插话与重发 → 换代并沿旧号结算 → 在回合边界切换新旧入口。
void fact_binding(const fs::path& root) {
    using namespace dfy::app;
    using namespace dfy::model;
    using Json = nlohmann::json;
    std::atomic<unsigned> calls = 0;
    // 从每次实际请求核对同一绑定的连续性以及切换入口后的可见边界。
    // 步骤：识别旧入口快照 → 核对长工作和原话 → 核对换代或回退后的基线。
    Application app({},root,root.parent_path()/"fact-data",Backend{[&](const auto&,const Request& request) {
        if (request.messages.back().content == "legacy snapshot") {
            check(request.messages.size() == 1,"legacy fallback retained the previous projection");
            ++calls;
            return Response{{Role::assistant,"internal"}};
        }
        const auto frame = Json::parse(request.messages.back().content);
        const auto seq = frame.at("factFrame").at("sequence").get<unsigned>();
        if (frame.at("factFrame").at("epoch")==3)
            check(request.messages.size()==1 && !frame.contains("hostWorkReceipts"),"retired work leaked into a new actor's model");
        if (seq == 11) {
            unsigned seen = 0;
            for (const auto& message : request.messages) if (message.role == Role::user) {
                const auto data = Json::parse(message.content);
                for (const auto& work : data.value("hostWorkReceipts",Json::array()))
                    if (work.at("work") == "W17" && work.at("effect").at("inBag") == 3) ++seen;
            }
            check(seen == 1 &&
                request.messages.front().content.find("eight cobblestones") != std::string::npos,
                "ninth intervening utterance lost unfinished goal, work or inventory");
        }
        if (frame.at("factFrame").at("epoch") == 2 && seq == 3) {
            const auto& works = frame.at("hostWorkReceipts");
            unsigned previews = 0, bodies = 0;
            for (const auto& work : works) {
                check(work.contains("work") && work.contains("accepted") && work.contains("exited") && work.contains("effectReceipt"),
                    "large baseline dropped a work identity or phase");
                const auto& effect = work.at("effect");
                if (effect.contains("sample")) ++bodies;
                else if (effect.at("bytes")==993 && effect.at("sha256").get_ref<const std::string&>().size()==64) ++previews;
            }
            check(request.messages.size()==1 && request.messages.back().content.size()<=128*1024 && works.size()==120 &&
                bodies>0 && previews>0 && bodies+previews==120 && frame.at("goal")=="follow up","large baseline lost facts or did not fit");
        } else if (frame.at("factFrame").at("epoch") == 2) {
            check(!frame.contains("hostWorkReceipts") && request.messages.size() == 1,"rebound leaked old facts");
            if (seq == 2) check(frame.at("factFrame").at("mode") == "baseline" && frame.at("new") == "world" &&
                frame.at("again") == true,"return from legacy omitted the current baseline");
        }
        ++calls; return Response{{Role::assistant,"internal"}};
    }});
    check(!app.plugins().load("test.mail"),"fact test owner unavailable");
    auto& agents = app.subagents();
    const auto plugin = app.plugins().instance("test.mail");
    const auto owner = plugin->generation(), id = agents.open(plugin);
    const auto make = [](unsigned epoch,unsigned seq,bool baseline,Json fields) {
        SubagentInput input{Json{{"set",std::move(fields)},{"closed",Json::array()}}.dump()};
        input.frame = dfy::plugins::SubagentFrame{epoch,seq,epoch == 1 ? "player/world-A" : "player/world-B",baseline};
        return input;
    };
    const auto settle = [&] {
        const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while (agents.busy(owner) && std::chrono::steady_clock::now() < end) app.advance_plugins();
        return agents.status(owner,id).phase;
    };
    auto base = make(1,1,true,{{"goal","eight cobblestones"},{"Q4","which chest"}});
    check(agents.submit(owner,id,base) == 1 && agents.submit(owner,id,base) == 1 &&
        settle() == SubagentPhase::succeeded,"duplicate queued baseline dispatched twice");
    bool conflict = false;
    try { agents.submit(owner,id,make(1,1,true,{{"goal","other"}})); } catch (const std::invalid_argument&) { conflict = true; }
    check(conflict,"same source and revision accepted conflicting content");
    bool gap = false;
    try { agents.submit(owner,id,make(1,3,false,{})); } catch (const dfy::plugins::SubagentGap& err) { gap = err.expected == 2; }
    check(gap,"missing fact sequence did not identify recoverable position");
    const auto accepted = agents.settle(owner,id,{1,1,"W17",R"({"source":1})",DFY_V2_WORK_ACCEPTED});
    const dfy::plugins::SubagentWorkFact effect{1,2,"W17",R"({"inBag":3})",DFY_V2_WORK_EFFECT};
    const auto applied = agents.settle(owner,id,effect);
    check(applied > accepted && agents.settle(owner,id,effect) == applied,"duplicate receipt advanced original work");
    for (unsigned seq = 2; seq <= 11; ++seq) {
        auto frame = make(1,seq,false,{{"utterance",seq}});
        check(agents.submit(owner,id,frame) == seq && settle() == SubagentPhase::succeeded,"intervening fact failed");
    }
    auto invalid = make(2,1,true,{{"new","world"}});
    invalid.tools = {"missing/tool"};
    agents.submit(owner,id,invalid);
    check(settle() == SubagentPhase::failed && calls == 11,"invalid baseline ran a model");
    agents.submit(owner,id,make(1,12,false,{{"still","old world"}}));
    check(settle() == SubagentPhase::succeeded && calls == 12,"failed replacement invalidated old session");
    auto rebound = make(2,1,true,{{"new","world"}});
    const auto original = agents.submit(owner,id,rebound);
    SubagentInput legacy{"legacy snapshot"};
    legacy.replace_history = true;
    bool busy = false;
    try { agents.submit(owner,id,legacy); } catch (const std::logic_error&) { busy = true; }
    check(busy,"legacy request replaced an admitted turn");
    check(settle() == SubagentPhase::succeeded && calls == 13,"valid rebound failed");
    const auto exited = agents.settle(owner,id,{1,3,"W17",R"({"exited":true})",DFY_V2_WORK_EXIT});
    check(agents.settle(owner,id,{1,4,"W17",R"({"picked":3})",DFY_V2_WORK_EFFECT}) > exited,"late original receipt lost");
    bool stale = false;
    try { agents.settle(owner,id,{1,1,"new-old-work","{}",DFY_V2_WORK_ACCEPTED}); }
    catch (const std::invalid_argument&) { stale = true; }
    check(stale,"retired binding started a new work identity");
    agents.submit(owner,id,legacy);
    check(settle() == SubagentPhase::succeeded && calls == 14,"legacy fallback missed its independent turn");
    agents.submit(owner,id,make(2,2,false,{{"again",true}}));
    check(settle() == SubagentPhase::succeeded && calls == 15 && agents.submit(owner,id,rebound) == original &&
        !agents.busy(owner),"return to fact mode lost prior admission or dispatched twice");

    const auto body = Json{{"sample",std::string(980,'x')}}.dump();
    for (unsigned index=0;index<120;++index) {
        const auto work = "read-"+std::to_string(index);
        agents.settle(owner,id,{2,1,work,R"({"skill":"observe","sourceMessage":1})",DFY_V2_WORK_ACCEPTED});
        agents.settle(owner,id,{2,2,work,body,DFY_V2_WORK_EFFECT});
        agents.settle(owner,id,{2,3,work,R"({"exited":true})",DFY_V2_WORK_EXIT});
    }
    agents.submit(owner,id,make(2,3,true,{{"goal","follow up"},{"largeField",std::string(32*1024,'a')}}));
    check(settle()==SubagentPhase::succeeded && calls==16,"long work baseline prevented next decision");
    const auto fixture = plugin->query<dfy_test_mailbox_api>(DFY_TEST_MAILBOX_INTERFACE,1);
    check(fixture && plugin->invoke(*fixture,[&](const auto& api,auto* instance) { return api.work_receipts(instance,id,2); })==DFY_V2_OK &&
        calls==16 && !agents.busy(owner),"ordered C ABI did not read the original receipt without dispatch");

    agents.close(owner,id);
    app.advance_plugins();
    bool closed = false;
    try { agents.status(owner,id); } catch (const std::invalid_argument&) { closed = true; }
    check(closed && !agents.busy(owner),"retired session retained an executable entry");
    const auto new_session = agents.open(plugin);
    const SubagentWorkFact late{2,4,"read-1",R"({"recipientPicked":8})",DFY_V2_WORK_EFFECT,0,true};
    const auto late_receipt = agents.settle(owner,id,late);
    check(agents.settle(owner,id,late)==late_receipt && Json::parse(agents.read_receipt(owner,id,2,late_receipt).value()).at("body").at("recipientPicked")==8 &&
        !agents.read_receipt(owner,new_session,2,late_receipt) && !agents.read_receipt(owner+1,id,2,late_receipt) && calls==16,
        "late receipt did not remain in the original retired owner and session");
    const auto refuses = [&](auto target,const SubagentWorkFact& fact) {
        try { agents.settle(owner,target,fact); } catch (const std::invalid_argument&) { return true; }
        return false;
    };
    check(refuses(new_session,late) && refuses(id,{2,1,"old-new","{}",DFY_V2_WORK_ACCEPTED}) &&
        agents.submit(owner,new_session,make(3,1,true,{{"world","next"}}))==1,"retired binding allowed a new work or crossed sessions");
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (agents.busy(owner) && std::chrono::steady_clock::now()<end) { app.advance_plugins(); std::this_thread::yield(); }
    check(calls==17 && agents.status(owner,new_session).phase==SubagentPhase::succeeded,"late settlement interfered with new session");
}

// 老绑定持续等回执时限制数据占用，新绑定仍能问答；补齐一项后按原准入再试。
// 步骤：连续退役十六个有未决工作的会话 → 打开新会话并核对工作满额 → 沿旧身份结算 → 核对重试和卸载。
void retired_capacity(const fs::path& root) {
    using namespace dfy::app;
    using namespace dfy::model;
    Application app({},root,root.parent_path()/"retired-facts",Backend{[](const auto&,const Request&) { return Response{{Role::assistant,"ok"}}; }});
    check(!app.plugins().load("test.mail"),"retired work owner unavailable");
    const auto plugin = app.plugins().instance("test.mail");
    const auto owner = plugin->generation();
    auto& agents = app.subagents();
    std::vector<std::uint64_t> sessions;
    for (unsigned n=0;n<=16;++n) {
        const auto id = agents.open(plugin);
        sessions.push_back(id);
        SubagentInput baseline{R"({"set":{"world":"fresh"},"closed":[]})"};
        baseline.frame = SubagentFrame{id,1,"isolated",true};
        agents.submit(owner,id,std::move(baseline));
        const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while (agents.busy(owner) && std::chrono::steady_clock::now()<end) { app.advance_plugins(); std::this_thread::yield(); }
        check(agents.status(owner,id).phase==SubagentPhase::succeeded,"retired data consumed an executable session slot");
        if (n<16) {
            agents.settle(owner,id,{id,1,"same-work","{}",DFY_V2_WORK_ACCEPTED,1});
            agents.close(owner,id);
            app.advance_plugins();
        }
    }
    const auto old = sessions.front(), fresh = sessions.back();
    const SubagentWorkFact next{fresh,1,"same-work","{}",DFY_V2_WORK_ACCEPTED,1};
    bool full = false;
    try { agents.settle(owner,fresh,next); } catch (const std::length_error&) { full = true; }
    check(full && agents.read_receipt(owner,old,old,1),"new admission silently evicted unsettled work");
    agents.settle(owner,old,{old,2,"same-work","{}",DFY_V2_WORK_EFFECT,0,true});
    agents.settle(owner,old,{old,3,"same-work","{}",DFY_V2_WORK_EXIT});
    check(agents.settle(owner,fresh,next)==1 && !agents.read_receipt(owner,old,old,1) &&
        agents.read_receipt(owner,sessions[1],sessions[1],1),"reclaiming final work affected another original binding");
    check(app.plugins().unload("test.mail")==ShutdownResult::closed && !agents.read_receipt(owner,fresh,fresh,1),
        "unload retained a revoked receipt route");
}

// 验证同一插件的子会话没有共享历史，且提交与模型派发分离。
// 步骤：建立两个会话及释放前取消 → 推进固定回答 → 分别核对历史与身份。
void subagents(const fs::path& root) {
    using namespace dfy::model;
    using namespace dfy::app;
    std::atomic<unsigned> calls{0};
    Application app({},root,root.parent_path()/"subagent-data",Backend{[&](const auto&,const Request& request) {
        const auto& last = request.messages.back();
        check(last.source==MessageSource::plugin,"subagent envelope was attributed to a human");
        if (last.content=="<status>x</status>")
            check(wire_content(last).find("&lt;status&gt;x&lt;/status&gt;")!=std::string::npos,
                "plugin snapshot exposed external markup");
        ++calls;
        return Response{{Role::assistant,std::to_string(request.messages.size())}};
    }});
    check(!app.plugins().load("test.mail"),"could not start subagent owner");
    const auto instance = app.plugins().instance("test.mail");
    auto& agents = app.subagents();
    const auto owner = instance->generation();
    const auto a = agents.open(instance), b = agents.open(instance);
    check(agents.submit(owner,a,{"<status>x</status>"}) == 1 && agents.busy(owner) && calls == 0,"queued turn dispatched inline");
    check(agents.submit(owner,b,{"cancelled"}) == 1 && agents.cancel(owner,b,1),"queued cancellation failed");
    bool denied = false;
    try { (void)agents.status(next_domain(),a); } catch (const std::invalid_argument&) { denied = true; }
    check(denied,"another owner read an isolated session");
    // 等待当前回合实际退出并返回值结果。
    const auto settle = [&](auto session) {
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            app.advance_plugins();
            const auto state = agents.status(owner,session);
            if (state.phase == SubagentPhase::succeeded) return std::string(state.answer);
            std::this_thread::yield();
        }
        throw std::runtime_error("subagent did not settle");
    };
    check(settle(a) == "1" && calls == 1,"first session or queued cancellation was incorrect");
    check(agents.submit(owner,a,{"second"}) == 2 && settle(a) == "3","child context did not persist");
    check(agents.submit(owner,b,{"independent"}) == 2 && settle(b) == "1","child context crossed sessions");
    {
        const auto controller = instance->query<dfy_test_mailbox_api>(DFY_TEST_MAILBOX_INTERFACE,1);
        std::uint64_t request = 0;
        check(controller && instance->invoke(*controller,[&](const auto& api, auto* plugin) {
            return api.ask(plugin,&request,nullptr);
        }) == DFY_V2_OK && request == 1 && calls == 3,"native ABI dispatched before leaving plugin callback");
    }
    check(app.plugins().unload("test.mail") == ShutdownResult::busy,"queued native subagent allowed unload");
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
    do { app.advance_plugins(); std::this_thread::yield(); }
    while (agents.busy(owner) && std::chrono::steady_clock::now() < deadline);
    {
        const auto controller = instance->query<dfy_test_mailbox_api>(DFY_TEST_MAILBOX_INTERFACE,1);
        check(controller && instance->invoke(*controller,[](const auto& api, auto* plugin) -> uint32_t {
            dfy_v2_agent_status status{};
            return !api.agent_result(plugin,&status,nullptr) && status.phase == DFY_V2_AGENT_SUCCEEDED &&
                std::string_view(status.answer.data,status.answer.length) == "1" ? DFY_V2_OK : DFY_V2_FAILED;
        }) == DFY_V2_OK,"native subagent did not yield an isolated result");
    }
    app.conversation().close();
    check(app.close_plugins() == ShutdownResult::pending,"shutdown did not account for child sessions");
    app.advance_plugins();
    check(agents.empty() && app.close_plugins() == ShutdownResult::closed,"child sessions did not release before shutdown");
}

// 用真实插件信箱验证被动审查与分类触发共享可信来源。
// 步骤：检查无信号时保留来信 → 主动处理且不启动模型 → 指定分类到达后启动原会话。
void main_inbox(const fs::path& root) {
    using namespace dfy::model;
    using namespace dfy::app;
    std::atomic<unsigned> calls{0};
    Application app({},root,root.parent_path()/"intake-data",Backend{[&](const auto&,const Request& input) {
        ++calls;
        return Response{{Role::assistant,input.messages.back().content}};
    }});
    check(!app.plugins().load("test.mail"),"mail route sender was not loaded");
    const auto instance = app.plugins().instance("test.mail");
    const auto& mail = app.mailbox_host();
    const auto address = instance->mailbox_addresses();
    const auto body = mailbox_text("test.request",1,"source",64);
    // 推进正式调度直到实际 DLL 向本体投递预期回件。
    const auto send = [&](uint64_t sequence) {
        mail.network->deliver(mail.identity,{address.inbound},sequence,body);
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            app.advance_plugins();
            if (mail.network->front(mail.identity,{address.outbound}) || app.conversation().busy()) return;
            std::this_thread::yield();
        }
        throw std::runtime_error("mailbox input did not arrive");
    };
    auto& intake = app.mailbox_intake();
    intake.watch({address.outbound},{},[&](const auto& delivery) -> std::optional<MailboxProjection> {
        check(delivery.sender == instance->generation() && delivery.event.id.sequence == 1,"passive review lost identity");
        return {};
    });
    send(1);
    check(!app.conversation().busy() && !calls && intake.review({address.outbound}) == MailboxIntakeResult::handled,
        "passive route incorrectly woke the model");
    const MailboxCategory wake[] = {{"test.reply",1}};
    intake.watch({address.outbound},wake,[](const auto& delivery) -> std::optional<MailboxProjection> {
        return MailboxProjection{std::string(delivery.event.payload->find("text")->as_string())};
    });
    send(2);
    check(app.conversation().busy() && app.conversation().status()->source == dfy::agent::RunOrigin::mailbox,
        "configured arrival did not reach the main Agent");
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
    std::optional<CompletedTurn> result;
    while (app.conversation().busy() && std::chrono::steady_clock::now() < deadline) {
        if (auto value = app.conversation().advance()) result = std::move(value);
        std::this_thread::yield();
    }
    check(result && result->answer.find("test.reply") != std::string::npos && result->answer.find("received") != std::string::npos &&
        calls == 1,"main inbox lost its category or payload");
}

// 验证真实 DLL 能阻断新分派及修改模型副本，原箱身份和绑定内容受保护。
// 步骤：发出两个原生事件 → 复核上下文和原信封 → 尝试拒绝受保护的阶段。
void transition_hooks(const fs::path& root, const fs::path& dll) {
    using namespace dfy::model;
    using namespace dfy::app;
    const auto packages = root/"transition-hooks";
    converter(packages,dll,"test.transition",'t');
    Application app({},packages,root/"transition-data",Backend{[](const auto&,const Request& input) {
        return Response{{Role::assistant,input.messages.back().content}};
    }});
    auto messages = std::make_shared<MessageStore>(root/"transition-data");
    app.bind_messages(messages,"transition-fixture",nullptr);
    check(!app.plugins().load("test.transition"),"system transition fixture failed to load");
    const auto instance = app.plugins().instance("test.transition");
    {
        const auto api = instance->query<dfy_test_mailbox_api>(DFY_TEST_MAILBOX_INTERFACE,1);
        check(api && instance->invoke(*api,[](const auto& api, auto* plugin) {
            const auto denied = api.emit_notice(plugin,1,1);
            return denied ? denied : api.emit_notice(plugin,2,0);
        }) == DFY_V2_OK,"native events were not captured for transition Hooks");
    }
    app.advance_plugins();
    check(!app.conversation().busy(),"vetoed native event still reached the main Agent");
    app.advance_plugins();
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
    std::optional<CompletedTurn> result;
    while (app.conversation().busy() && std::chrono::steady_clock::now() < deadline) {
        if (auto completed = app.conversation().advance()) result = std::move(completed);
        std::this_thread::yield();
    }
    check(result && !result->error,"allowed event did not run the main Agent");
    const auto prompt = nlohmann::json::parse(result->answer);
    check(prompt.at("category") == "test.notice" && prompt.at("sender") == instance->generation() &&
        prompt.at("content") == "adjusted","context Hook lost the original mailbox envelope");
    const auto decision = app.plugins().transition_hook({DFY_V2_TRANSITION_MAIL_CONTEXT,0,0,0,
        instance->generation(),1,1,0,0,0,{},{}});
    check(decision.allowed && !decision.value && decision.observed == 1 && decision.errors == 1,
        "read-only Hook could reject a bound context");
    {
        const auto api = instance->query<dfy_test_mailbox_api>(DFY_TEST_MAILBOX_INTERFACE,1);
        check(api && instance->invoke(*api,[](const auto& api, auto* plugin) {
            return api.transition_count(plugin,DFY_V2_TRANSITION_EVENT_CAPTURED) == 2 &&
                api.transition_count(plugin,DFY_V2_TRANSITION_EVENT_DECODED) == 2 &&
                api.transition_count(plugin,DFY_V2_TRANSITION_EVENT_DISPATCHED) == 2 &&
                api.transition_count(plugin,DFY_V2_TRANSITION_RUN_EXITED) == 1;
        }),"observers did not see the actual application boundaries without Trace");
        dfy::management::TerminalHost terminals;
        const auto command = [&](const std::string& client, nlohmann::json body) {
            (void)terminals.exchange(client,body);
            terminals.advance(app);
            return terminals.exchange(client,body);
        };
        (void)command("keyboard",{{"id",1u},{"action","open"},{"interactive",true}});
        (void)command("pipe",{{"id",1u},{"action","open"},{"interactive",false}});
        terminals.completed(*result,true);
        check(instance->invoke(*api,[](const auto& api, auto* plugin) { return api.emit_notice(plugin,3,2); }) == DFY_V2_OK,
            "ASK event failed to reach the host");
        app.advance_plugins();
        const auto ask = app.console_ask();
        const auto keyboard = command("keyboard",{{"id",2u},{"action","read"},{"after",0u}});
        const auto pipe = command("pipe",{{"id",2u},{"action","read"},{"after",0u}});
        check(!result->user_facing && messages->watermark()==0 && pipe.at("out")=="" && keyboard.at("out")=="",
            "internal plugin settlement reached the chat or a terminal");
        check(ask && keyboard.at("ask").at("id")==ask->first && !pipe.contains("ask"),"ASK leaked to non-interactive pipe");
        try {
            (void)command("pipe",{{"id",3u},{"action","presented"},{"ask",ask->first}});
            throw std::runtime_error("pipe acknowledged another terminal's ASK");
        } catch (const std::invalid_argument&) {}
        check(command("keyboard",{{"id",3u},{"action","presented"},{"ask",ask->first}}).at("presented")==true &&
            command("keyboard",{{"id",4u},{"action","line"},{"text","/ask answer "+std::to_string(ask->first)+" yes"}}).at("err")=="" &&
            app.answer_console_ask(ask->first,"yes"),
            "read-only observation interrupted a presented question or its answer");
        app.advance_plugins();
        check(instance->invoke(*api,[](const auto& api, auto* plugin) {
            return api.transition_count(plugin,DFY_V2_TRANSITION_ASK_PRESENTED) == 1 &&
                api.transition_count(plugin,DFY_V2_TRANSITION_ASK_ANSWERED) == 1 &&
                api.transition_count(plugin,DFY_V2_TRANSITION_REPLY_ENQUEUED) == 1;
        }),"a duplicate answer caused additional notices or interrupted delivery");
    }
}
}

// 验证从正式插件管理器载入的 C DLL 可以收到本体请求并回信。
// 步骤：加载并定位信箱 → 投递与回信 → 检查实际分类 → 卸载并阻止旧地址复用。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 4,"mailbox DLL, workspace and status package required");
        const auto root = package(argv[2],argv[1]);
        PluginMailboxHost host{std::make_shared<Mailboxes>(MailboxLimits{2,2,4,64}),next_domain()};
        auto channel = std::make_shared<BroadcastChannel>(std::make_shared<EventStore>(8,64),2,2);
        PluginManager manager(root,root.parent_path()/"data",channel,1,host);
        const auto before_load = manager.capture_hooks();
        check(manager.load("test.mail") == DFY_V2_OK,"mailbox plugin failed to attach at load");
        const auto registered = manager.hook_version();
        check(registered.starts_with("hooks-v1:sha256:"),"registered policy lacked a public version");
        const auto instance = manager.instance("test.mail");
        const auto addresses = instance->mailbox_addresses();
        check(addresses.inbound && addresses.outbound,"manager lost registered mailbox addresses");
        const auto hooks = instance->hooks();
        const dfy_v2_hook_input control{DFY_V2_HOOK_TOOL_START,0,1,1,1,1,0,1,{}};
        check(hooks.size() == 2 && hooks[0].point == DFY_V2_HOOK_MODEL_REQUEST &&
            hooks[1].sessions == (DFY_V2_HOOK_MAIN_SESSION|DFY_V2_HOOK_OWN_SESSION) &&
            instance->decide_hook(1,control) == DFY_V2_HOOK_CONFIRM,"instance lost its C Hook registration");
        auto scoped = control;
        check(manager.control_hook(scoped) == DFY_V2_HOOK_CONFIRM,"main session skipped its control Hook");
        scoped.owner = instance->generation();
        check(manager.control_hook(scoped) == DFY_V2_HOOK_CONFIRM,"owned session skipped its control Hook");
        scoped.owner = next_domain();
        check(manager.control_hook(scoped) == DFY_V2_HOOK_ALLOW,"Hook exceeded its registered scope");
        {
            const auto active = manager.capture_hooks();
            check(before_load.version == "none" && before_load.control(control) == DFY_V2_HOOK_ALLOW &&
                active.version == registered && active.control(control) == DFY_V2_HOOK_CONFIRM,
                "active run changed its Hook plan across a plugin load");
            check(manager.unload("test.mail") == ShutdownResult::in_use && active.control(control) == DFY_V2_HOOK_CONFIRM,
                "unload invalidated the controller held by an active run");
        }

        const auto body = mailbox_text("test.request",1,"source",64);
        check(host.network->deliver(host.identity,{addresses.inbound},1,body).state == DeliveryState::accepted,
            "host request was not admitted");
        {
            const auto reply = instance->query<dfy_test_mailbox_api>(DFY_TEST_MAILBOX_INTERFACE,1);
            check(reply && instance->invoke(*reply,[](const auto& api, auto* plugin) { return api.reply(plugin,nullptr); }) == DFY_V2_OK,
                "actual C plugin could not poll and post");
        }
        auto answer = host.network->read(host.identity,{addresses.outbound});
        check(answer.event.get().payload->descriptor()->name() == "test.reply" &&
            answer.event.get().payload->find("text")->as_string() == "received","host received invalid reply");
        check(host.network->deliver(host.identity,{addresses.inbound},2,body).state == DeliveryState::accepted,
            "activity request was not admitted");
        check(manager.advance() == RunResult::executed && host.network->read(host.identity,{addresses.outbound}).event.get().id.sequence == 2,
            "scheduler did not advance the plugin's independent loop");
        check(manager.unload("test.mail") == ShutdownResult::closed && !instance->mailbox_addresses().inbound,"mailbox blocked clean unload");
        check(manager.control_hook(control) == DFY_V2_HOOK_ALLOW && manager.hook_version() == "none","unloaded Hook remained active or versioned");
        check(manager.load("test.mail") == DFY_V2_OK && manager.instance("test.mail")->mailbox_addresses().outbound != addresses.outbound,
            "reload reused old mailbox routing");
        check(manager.hook_version() == registered,"instance generation changed the public registration version");
        check(manager.close(),"mailbox manager did not close");
        main_hook(root,argv[3]);
        subagents(root);
        main_inbox(root);
        text_hooks(root.parent_path(),argv[1]);
        run_text_hooks(root.parent_path(),argv[1],argv[3]);
        fact_binding(root);
        retired_capacity(root);
        transition_hooks(root.parent_path(),argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
