#include "../decision_input.hpp"
#include <dafeiyu/plugin_manager.hpp>
#include "application.hpp"
#include "run_log.hpp"
#include "ask.hpp"
#include "trace_commands.hpp"
#include "../../plugins/examples/observer/observer_api.h"
#include <dafeiyu/diagnostic_json.hpp>
#include <dafeiyu/run_loop.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <semaphore>
#include <thread>
namespace {
using namespace dfy::runtime;
using namespace dfy::plugins;
namespace fs = std::filesystem;
// 在完整业务链、事件消费或包生命周期违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 为完整链路建立唯一目录，不依赖源码根中的插件或用户配置。
fs::path workspace(const fs::path& root) {
    fs::create_directories(root);
    for (unsigned i = 1;; ++i) { const auto path = root/std::to_string(i); if (fs::create_directory(path)) return path; }
}
// 在受控原生调用内查询独立观察包的统计，返回前归还临时代码持有。
dfy_observer_stats statistics(PluginManager& manager) {
    auto code = manager.instance("example.observer")->retain_code(); auto call = code.enter();
    const auto query = reinterpret_cast<dfy_v2_query_fn>(call.symbol(DFY_V2_QUERY_EXPORT));
    const dfy_v2_plugin_api* bootstrap = nullptr; const void* table = nullptr;
    check(query(2,&bootstrap,nullptr) == DFY_V2_OK && bootstrap && bootstrap->query_interface(
        {DFY_OBSERVER_STATS_INTERFACE,sizeof(DFY_OBSERVER_STATS_INTERFACE)-1},1,&table,nullptr) == DFY_V2_OK && table,"integration observer stats missing");
    const auto& api = *static_cast<const dfy_observer_stats_api*>(table);
    check(api.header.struct_size == sizeof(api) && api.header.interface_version == 1 && api.read,"integration observer stats layout wrong");
    dfy_observer_stats result{}; check(api.read(&result,nullptr) == DFY_V2_OK,"integration observer stats failed"); return result;
}
// 持续推进真实宿主及后台模型，观察任务不会阻挡问答的业务完成。
void finish(dfy::agent::Session& session, PluginManager& plugins, const std::shared_ptr<dfy::agent::TurnRequest>& request) {
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (request->exit()->state() == OperationState::pending && std::chrono::steady_clock::now() < deadline) {
        session.advance(); check(plugins.advance() != RunResult::failed,"integration plugin callback failed"); std::this_thread::yield();
    }
    check(request->exit()->state() == OperationState::succeeded,"integration model did not exit");
    for (unsigned i = 0; i < 8; ++i) check(plugins.advance() != RunResult::failed,"integration observer drain failed");
}
struct Release {
    std::promise<void>& promise; bool released = false;
    // 异常退出也唤醒被控制的模型线程，避免测试留下后台等待。
    ~Release() { if (!released) promise.set_value(); }
    // 一次解除正在进行的模型调用。
    void open() { promise.set_value(); released = true; }
};

// 在正式应用装配中验证空插件问答与关闭顺序。
// 步骤：注入固定回答 → 接纳并检查活动时关闭拒绝 → 等待实际退出 → 关闭插件管理。
void shared_application(const fs::path& run) {
    dfy::app::Application application({},run/"shared-plugins",run/"shared-user",dfy::model::Backend{
        [](const auto&, const auto& request) {
            check(request.messages.front().role == dfy::model::Role::system &&
                request.messages.front().content.starts_with("【职责与信息来源 v1】") &&
                request.messages.front().content.find("【人格与风格 v1】\n你叫大肥鱼。")!=std::string::npos,"default persona was not composed for direct dialogue");
            const auto& last = dfy::test::business_messages(request).back();
            check(last.source==dfy::model::MessageSource::human &&
                dfy::model::wire_content(last).find("&lt;status&gt;x&lt;/status&gt;")!=std::string::npos,
                "direct dialogue lost its user origin or exposed external markup");
            return dfy::model::Response{{dfy::model::Role::assistant,"shared:"+last.content}};
        }},dfy::app::ApplicationRunConfig{.trace_directory = run/"direct-trace"});
    auto& conversation = application.conversation();
    const std::string prompt = "hello-<status>x</status>"+std::string(20*1024,'a');
    const auto accepted = application.submit_dialogue(prompt);
    const auto& display = conversation.display();
    check(display.turns.size() == 1 && display.turns.front().prompt == prompt &&
        display.turns.front().result == OperationState::pending && display.turns.front().answer.empty(),"accepted dialogue not displayed");
    bool busy = false;
    try { (void)application.submit_dialogue("another line"); } catch (const std::logic_error&) { busy = true; }
    check(busy,"mailbox changed direct chat busy semantics");
    check(application.close_plugins() == ShutdownResult::busy,"shared application ended plugins before its conversation");

    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    std::optional<dfy::app::CompletedTurn> result;
    while (conversation.busy() && std::chrono::steady_clock::now() < deadline) {
        if (auto latest = conversation.advance()) result = std::move(latest);
        std::this_thread::yield();
    }
    check(result && result->answer == "shared:"+prompt && result->status.turn == accepted.turn &&
        conversation.status()->exit == OperationState::succeeded,"shared application lost result or actual exit");
    std::ostringstream observed, replay;
    const dfy::agent::RunId id{accepted.session,accepted.turn};
    check(display.turns.size() == 1 && display.turns.front().run == id && display.turns.front().answer == result->answer &&
        display.turns.front().result == OperationState::succeeded && display.turns.front().exit == OperationState::succeeded,
        "display lost the first ordinary reply or exit");
    const auto revision = display.revision;
    check(!conversation.advance() && display.revision == revision &&
        dfy::agent::encode_diagnostic(*conversation.diagnostic()).find(prompt) == std::string::npos,
        "repeated advance changed visible completion or public diagnostic published text");
    dfy::app::review_trace(application.trace_file(),id,observed);
    dfy::app::replay_trace(application.trace_file(),id,replay);
    check(observed.str().find("\"kind\":\"mail.dispatched\"") != std::string::npos &&
        observed.str().find("\"category\":\"dialogue.text\"") != std::string::npos &&
        observed.str().find("\"personaProfile\":\"dafeiyu/default/2\"") != std::string::npos &&
        conversation.diagnostic()->timeline.versions.persona_profile == "dafeiyu/default/2" &&
        replay.str().find("1 model requests, 0 tool exits, succeeded") != std::string::npos,
        "direct mailbox was not linked to its replayed model run");

    conversation.close();
    check(conversation.closed() && application.close_plugins() == ShutdownResult::closed,"shared application could not finish shutdown");
}

// 验证可见记录按原回合结算失败和取消，并限制留存正文及条数。
// 步骤：阻塞后取消 → 解除阻塞并确认退出 → 连续失败压过条数和字节上限 → 清空。
void display_outcomes(const fs::path& run) {
    std::promise<void> release, entered;
    auto gate = release.get_future().share();
    auto started = entered.get_future();
    dfy::app::Application application({},run/"display-plugins",run/"display-user",dfy::model::Backend{
        // 将取消场景保持在途，其他请求返回固定失败。
        // 步骤：匹配指定原话 → 等待放行或抛出替身错误 → 返回迟到文本。
        [&](const auto&, const auto& request) -> dfy::model::Response {
            if (dfy::test::business_messages(request).back().content == "visible cancelled prompt") {
                entered.set_value(); gate.wait();
                return {{dfy::model::Role::assistant,"late private answer"}};
            }
            throw std::runtime_error("private backend failure");
        }});
    Release cleanup{release};
    auto& conversation = application.conversation();
    const auto& display = conversation.display();
    // 有界推进原会话，等待业务线程确认实际退出。
    // 步骤：固定期限 → 推进等待 → 断言会话空闲。
    const auto finish = [&] {
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while (conversation.busy() && std::chrono::steady_clock::now() < deadline) { conversation.advance(); std::this_thread::yield(); }
        check(!conversation.busy(),"display fixture did not exit");
    };
    const auto cancelled = application.submit_dialogue("visible cancelled prompt");
    check(started.wait_for(std::chrono::seconds(2)) == std::future_status::ready && conversation.cancel(),"display test could not cancel pending request");
    const auto completion = conversation.advance();
    check(completion && display.turns.size() == 1 && display.turns.back().run.sequence == cancelled.turn &&
        display.turns.back().result == OperationState::cancelled && display.turns.back().exit == OperationState::pending &&
        display.turns.back().prompt == "visible cancelled prompt" && display.turns.back().answer.empty(),"display lost cancel before actual exit");
    cleanup.open(); finish();
    check(display.turns.back().exit == OperationState::succeeded && display.turns.back().answer.empty(),"display replaced cancelled result with late text");

    for (unsigned i = 0; i < 17; ++i) { application.submit_dialogue("visible failed prompt"); finish(); }
    check(display.turns.size() == 16 && display.turns.front().run.sequence > cancelled.turn &&
        display.turns.back().prompt == "visible failed prompt" && display.turns.back().result == OperationState::failed &&
        display.turns.back().answer.empty(),"display retained excess turns or published failure text");
    for (unsigned i = 0; i < 5; ++i) { application.submit_dialogue(std::string(512*1024,'x')); finish(); }
    check(display.turns.size() == 4 && display.turns.front().prompt.size() == 512*1024,"display retained text past its byte budget");
    (void)conversation.clear();
    check(display.turns.empty(),"successful session clear retained display text");
}

// 使用真实主会话验证排队身份、容量、停止收尾、失败恢复和清空后的去向。
// 步骤：阻塞原轮并压满队列 → 停止并提前恢复 → 核对逐条新轮 → 清空后恢复剩余原话。
void dialogue_queue(const fs::path& run) {
    using dfy::app::QueueAdmission;
    std::binary_semaphore entered(0), release(0);
    dfy::app::Application application({},run/"queue-plugins",run/"queue-user",dfy::model::Backend{
        // 按原话保持首次请求在途、制造一次失败或回复实际收到的最后一句。
        // 步骤：识别受控输入 → 依场景等待或失败 → 返回可核对的原话。
        [&](const auto&, const auto& request) -> dfy::model::Response {
            const auto& text = dfy::test::business_messages(request).back().content;
            if (text == "当前原话") {
                entered.release();
                if (!release.try_acquire_for(std::chrono::seconds(5))) throw std::runtime_error("fixture release timed out");
            }
            if (text == "固定失败") throw std::runtime_error("fixed queued failure");
            return {{dfy::model::Role::assistant,text}};
        }});
    auto& conversation = application.conversation();
    const auto& display = conversation.display();
    const auto original = application.submit_dialogue("当前原话");
    const dfy::agent::RunId after{original.session,original.turn};
    check(entered.try_acquire_for(std::chrono::seconds(2)),"original request did not enter fixture");
    check(application.enqueue_dialogue({after.session,after.sequence+1},"过期目标").state == QueueAdmission::stale &&
        application.enqueue_dialogue(after,"  ").state == QueueAdmission::rejected &&
        application.enqueue_dialogue(after,"\xff").state == QueueAdmission::rejected,"invalid queue target or input was admitted");

    const std::string large(dfy::app::dialogue_text_limit,'a');
    const auto large1 = application.enqueue_dialogue(after,large), large2 = application.enqueue_dialogue(after,large);
    check(large1.message && large2.message && application.enqueue_dialogue(after,"x").state == QueueAdmission::full,
        "queue exceeded its total text budget");
    check(application.remove_queued_dialogue(large1.message) && application.remove_queued_dialogue(large2.message),"queue did not recover text capacity");
    const auto first = application.enqueue_dialogue(after,"同样的原话").message;
    const auto failed = application.enqueue_dialogue(after,"固定失败").message;
    const auto last = application.enqueue_dialogue(after,"同样的原话").message;
    check(first && failed > first && last > failed,"independent queued messages lost their identities");
    while (display.queue.size() < dfy::app::dialogue_queue_limit)
        check(application.enqueue_dialogue(after,"可移除").state == QueueAdmission::accepted,"premature queue refusal");
    check(application.enqueue_dialogue(after,"已满").state == QueueAdmission::full && conversation.cancel(),"full queue blocked stop");
    conversation.advance(); application.advance_plugins();
    check(display.queue_paused && display.turns.size() == 1 && display.turns.front().exit == OperationState::pending,
        "cancelled active round did not pause pending items before exit");
    while (display.queue.size() > 3) check(application.remove_queued_dialogue(display.queue.back().message),"cannot remove tail item");
    check(!application.resume_dialogue_queue(last) && application.resume_dialogue_queue(first),"resume bypassed head identity");
    application.advance_plugins(); conversation.advance();
    check(display.queue.size() == 3 && display.turns.size() == 1,"queue started while cancelled request was still running");

    // 按应用常规次序推进，等待指定状态并保留主回合的实际退出。
    const auto until = [&](const auto& done) {
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while (!done() && std::chrono::steady_clock::now() < deadline) {
            application.advance_plugins(); conversation.advance(); std::this_thread::yield();
        }
        check(done(),"queue did not reach expected state");
    };
    release.release();
    until([&] { return display.queue_paused && !conversation.busy(); });
    check(display.turns.size() == 3 && display.turns[1].queued_message == first &&
        display.turns[1].answer == "同样的原话" && display.turns[2].queued_message == failed &&
        display.turns[2].result == OperationState::failed && display.queue.size() == 1 &&
        display.queue.front().message == last && !application.remove_queued_dialogue(first),"queue lost FIFO, result, or original identity");
    const auto failed_run = display.turns.back().run;
    conversation.clear();
    check(display.turns.empty() && display.queue_paused && display.queue.front().text == "同样的原话" &&
        application.resume_dialogue_queue(last),"idle clear lost the paused queue");
    until([&] { return display.queue.empty() && !conversation.busy(); });
    check(display.turns.size() == 1 && display.turns.front().queued_message == last &&
        display.turns.front().run.sequence > failed_run.sequence && display.turns.front().answer == "同样的原话",
        "resumed independent message did not get its own new turn");

    const auto& host = application.mailbox_host();
    const auto source = next_domain();
    const auto box = host.network->open(host.identity,source);
    application.mailbox_intake().watch(box,{},[](const auto&) { return dfy::app::MailboxProjection{"旁听消息"}; },
        dfy::app::MailboxPriority::normal,{},{},[](const auto&, auto) { throw std::runtime_error("fixed after admission"); });
    host.network->deliver_next(source,box,dfy::events::mailbox_text("fixture.note",1,"附带来信",128));
    const auto with_mail = application.enqueue_dialogue(display.turns.back().run,"带收件的新轮").message;
    until([&] { return display.queue.empty() && !conversation.busy(); });
    check(with_mail && display.turns.size() == 2 && display.turns.back().queued_message == with_mail &&
        display.turns.back().answer.find("带收件的新轮") != std::string::npos && !host.network->front(host.identity,box),
        "mail callback failure lost the accepted turn or redelivered its mail");
}

// 核对真人补充在正式应用的收件、显示和本机 Trace 指向实际送模步骤。
// 步骤：向原对话接纳补充 → 推进单一回合 → 比对显示、模型实收与 Trace。
void dialogue_inputs(const fs::path& run) {
    dfy::app::ApplicationRunConfig config;
    config.trace_directory = run/"input-trace";
    dfy::app::Application application({},run/"input-plugins",run/"input-user",dfy::model::Backend{
        [](const auto&,const auto& request) -> dfy::model::Response { return {{dfy::model::Role::assistant,"收到："+dfy::test::business_messages(request).back().content}}; }
    },config);
    const auto begun = application.submit_dialogue("第一句话");
    const dfy::agent::RunId id{begun.session,begun.turn};
    check(application.append_dialogue({id.session,id.sequence+1},"错轮").state==dfy::agent::InputAdmission::closed,"stale supplement altered session");
    const auto receipt = application.append_dialogue(id,"后到的原话");
    check(receipt.state==dfy::agent::InputAdmission::accepted && receipt.message,"supplement did not receive message identity");
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while (application.conversation().busy() && std::chrono::steady_clock::now()<deadline) {
        application.advance_plugins(); application.conversation().advance(); std::this_thread::yield();
    }
    const auto& turns = application.conversation().display().turns;
    check(turns.size()==1 && turns.front().run==id && turns.front().accepts_input && turns.front().answer=="收到：后到的原话" &&
        turns.front().inputs.size()==1 && turns.front().inputs.front().message==receipt.message &&
        turns.front().inputs.front().state==dfy::agent::InputState::submitted && turns.front().inputs.front().model_step==2,
        "display lost original message, run, or actual model step");
    std::ostringstream log;
    dfy::app::review_trace(application.trace_file(),id,log);
    check(log.str().find("dialogue.input_accepted")!=std::string::npos && log.str().find("dialogue.input_settled")!=std::string::npos &&
        log.str().find("\"modelStep\":2")!=std::string::npos && log.str().find("后到的原话")!=std::string::npos,
        "local trace cannot correlate accepted input to actual model step");
    std::ostringstream replay;
    dfy::app::replay_trace(application.trace_file(),id,replay);
    check(replay.str().find("2 model requests, 0 tool exits, succeeded")!=std::string::npos,"same-turn supplement could not be replayed");
    const auto altered = run/"altered-input-step.jsonl";
    {
        std::ifstream source(application.trace_file(),std::ios::binary);
        std::ofstream changed(altered,std::ios::binary);
        std::string line;
        while (std::getline(source,line)) {
            if (line.find("dialogue.input_settled") != line.npos) {
                const auto at = line.find("\"modelStep\":2"); check(at!=line.npos,"missing recorded model step");
                line.replace(at,13,"\"modelStep\":3");
            }
            changed << line << '\n';
        }
    }
    bool rejected = false;
    try { dfy::app::replay_trace(altered,id,replay); }
    catch (const std::runtime_error& error) { rejected = std::string_view(error.what())=="replay: dialogue input settlement differs"; }
    check(rejected,"replay accepted a supplement assigned to the wrong model step");
    for (const std::string mode : {"source","eventUtcMs","tailBody","legacy"}) {
        const auto path = run/("replay-"+mode+".jsonl");
        std::ifstream source(application.trace_file(),std::ios::binary);
        std::ofstream changed(path,std::ios::binary);
        std::string line;
        while (std::getline(source,line)) {
            auto row = nlohmann::json::parse(line);
            if (row.at("kind")=="model.prepared") {
                auto& messages = row.at("input").at("messages");
                if (mode=="legacy") {
                    messages.erase(messages.end()-1);
                    row.erase("messageEvidence"); row.erase("wireSha256");
                } else if (mode=="tailBody" && row.at("step")==2) {
                    auto status = nlohmann::json::parse(messages.back().at("content").get<std::string>());
                    status.at("asOf").at("event") = 1234;
                    messages.back().at("content") = status.dump();
                } else if (mode!="tailBody" && row.at("step")==2) row.at("messageEvidence").at(messages.size()-2)[mode] =
                    mode=="source" ? nlohmann::json("host_status") : nlohmann::json(0);
            }
            changed << row.dump() << '\n';
        }
        changed.close();
        bool differs = false;
        try { dfy::app::replay_trace(path,id,replay); }
        catch (const std::runtime_error& error) { differs = std::string_view(error.what()).find("model request 2 differs")!=std::string_view::npos; }
        check(differs==(mode!="legacy"),"replay missed changed source/time or rejected legacy trace");
    }
}

// 以同一台灯和受控的身体采样核对距离、屏风边缘、可知字段与实例隔离。
// 步骤：核对亲见及听距边界 → 改变屏风相交条件 → 拒绝外部事件及错误的角色身份。
void room_senses(const dfy::app::BuiltinRoom& room, dfy::app::RoomSelf resident, dfy::app::RoomSelf visitor) {
    using namespace dfy::app;
    const auto binding = resident.room;
    const auto observe = [&](RoomPoint point, bool on) {
        auto sample = resident;
        sample.position = point;
        return room.perceive_switch(sample,{binding,on});
    };
    const auto visible = [&](RoomPoint point, bool on) {
        const auto cue = observe(point,on);
        const auto* seen = cue ? std::get_if<SeenLampChange>(&*cue) : nullptr;
        return seen && seen->place == RoomPlace::desk && seen->on == on;
    };
    const auto audible = [&](RoomPoint point, bool on) {
        const auto cue = observe(point,on);
        const auto* heard = cue ? std::get_if<HeardSwitch>(&*cue) : nullptr;
        return heard && heard->direction == RoomPlace::desk;
    };
    check(visible(resident.position,true) && visible({2,0},false) && visible({1,0},true),"near lamp or inclusive sight threshold lost");
    check(audible({0,0},true) && !observe({-1,0},true) && !observe({-32768,-32768},false),
        "hearing threshold or distant sample wrong");
    check(audible({4,3},true) && audible({4,3},false) && audible({3,2},false) && audible({5,2},true) &&
        visible({2,2},true) && visible({6,2},false),"screen edge or sound-only lamp change wrong");

    check(!room.perceive_switch(resident,{visitor.room,true}) && !room.perceive_switch(visitor,{binding,true}),
        "percept crossed room instances");
    visitor.room = binding;
    check(!room.perceive_switch(visitor,{binding,false}),"percept crossed body bindings");
    auto stale = binding;
    ++stale.generation;
    check(!room.perceive_switch(resident,{stale,true}),"stale event generation produced a percept");
    resident.room = stale;
    check(!room.perceive_switch(resident,{binding,true}),"stale body sample produced a percept");
}

// 在无页面、空插件下核对同名书房、固定布局和自身位置的实例隔离。
// 步骤：创建两个应用 → 核对共有布局 → 分别复核身体与代次 → 验证查询零模型调用。
void builtin_room(const fs::path& run) {
    using namespace dfy::app;
    std::atomic<unsigned> calls = 0;
    dfy::model::Backend backend{[&](const auto&,const auto&) -> dfy::model::Response {
        ++calls; return {{dfy::model::Role::assistant,"意外的模型调用"}};
    }};
    Application first({},run/"empty-room-one",run/"room-user-one",backend);
    Application other({},run/"empty-room-two",run/"room-user-two",backend);
    const auto layout = first.room().layout(), second = other.room().layout();
    check(layout.name=="书房" && layout.name==second.name && layout.version==1 && layout.room!=second.room &&
        layout.places.size()==3 && layout.objects.size()==2,"builtin layout or instance identity missing");
    check(std::ranges::any_of(layout.passages,[](const auto& link) {
        return link.from==RoomPlace::desk && link.to==RoomPlace::window && link.distance>0;
    }) && std::ranges::any_of(layout.objects,[](const auto& object) {
        return object.id==RoomObject::desk_lamp && object.place==RoomPlace::desk && !object.use.empty();
    }),"known room omitted route or object use");
    const auto self = first.room().self(layout.room), another = other.room().self(second.room);
    check(self && another && self->body!=another->body && self->room==layout.room && self->place==RoomPlace::desk &&
        self->activity==RoomActivity::resting && self->revision==1 && !first.room().self(second.room) &&
        !first.room().self({layout.room.instance,layout.room.generation+1}),"self query crossed room binding or lost initial pose");
    room_senses(first.room(),*self,*another);
    const auto now = dfy::agent::ObservationTime{};
    const auto signal = first.room().capture_switch({layout.room,true},1,now);
    check(signal && first.room().receive(*signal,now).status==dfy::agent::ObservationAdmission::inserted &&
        first.room().observe_current(layout.room,self->body,now)->payload().find("on")->as_bool() &&
        first.room().percepts_since(layout.room,self->body,0)->cursor==1,"bound room did not retain its own signal");
    first.advance_plugins(); first.conversation().advance();
    check(!calls.load() && !first.conversation().status() && first.room().self(layout.room)->revision==self->revision,
        "read-only room query advanced the body or woke the model");
}

// 核对离线运行保留同名工具的原调用顺序和 Hook 实际使用的参数。
// 步骤：记录固定模型及有序改写 → 比对工具回执 → 脱离原 Hook 重新执行。
void trace_rewrites(const fs::path& run) {
    using namespace dfy::agent;
    using namespace dfy::model;
    constexpr const char* name = "read_cell";
    const RunId id{next_domain(),1};
    const auto trace = std::make_shared<dfy::app::RunLog>(run/"rewritten-trace");
    const auto capture = trace->sinks("fixture");
    std::vector<std::string> effective_arguments;
    ToolCatalog catalog;
    const std::vector<std::string> ids{"fixture/cell","fixture/second"};
    catalog.add({ids.front(),next_domain(),1,{name,"Read a cell",
        R"({"type":"object","properties":{"cell":{"type":"integer"}},"required":["cell"],"additionalProperties":false})"}},
        [&](const ToolInvocation& invocation) {
            effective_arguments.push_back(invocation.call.arguments);
            return completed_tool({true,ToolEffect::not_applied,invocation.call.arguments,ToolError::none});
        });
    // 第二个工具的模型名排在前面，重放仍须遵守原请求的实际目录顺序。
    catalog.add({ids.back(),next_domain(),1,{"aaa_lookup","Another cell",R"({"type":"object","properties":{},"additionalProperties":false})"}},
        [](const auto&) { return completed_tool({true,ToolEffect::not_applied,"unused",ToolError::none}); });
    RunOptions options;
    options.tools = catalog.snapshot(ids);
    options.model_capture = capture.model;
    options.compare_projection = true;
    options.tool_capture = capture.tool;
    options.exit_capture = capture.exit;
    options.text_hook = [](TextHookPoint point,const auto&,auto,std::string_view value,std::size_t) -> std::optional<std::string> {
        if (point == TextHookPoint::model_response) return std::string(value)+" after hook";
        if (point == TextHookPoint::tool_arguments) return value == R"({"cell":1})" ? R"({"cell":11})" : R"({"cell":22})";
        return {};
    };
    RunLoop loop(id,{},Backend{[&](const auto&,const Request& input) {
        if (dfy::test::business_messages(input).back().role == Role::user)
            return Response{{Role::assistant,"reading",{{"cell-1",name,R"({"cell":1})"},{"cell-2",name,R"({"cell":2})"}}},FinishReason::tool_calls};
        return Response{{Role::assistant,"finished"}};
    }},std::make_shared<MessageHistory>(),"read two cells",std::move(options));

    const auto request = loop.request();
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    while (request->exit()->state() == OperationState::pending && std::chrono::steady_clock::now() < deadline) {
        loop.advance();
        std::this_thread::yield();
    }
    check(request->exit()->state() == OperationState::succeeded && request->answer() == "finished after hook" &&
        effective_arguments == std::vector<std::string>{R"({"cell":11})",R"({"cell":22})"},"fixture did not use ordered rewrite results");

    std::ostringstream replay;
    dfy::app::replay_trace(trace->path(),id,replay);
    check(replay.str().find("2 model requests, 2 tool exits, succeeded") != std::string::npos,
        "replay lost response rewrite or effective tool arguments");
    std::ifstream rows(trace->path(),std::ios::binary);
    std::vector<nlohmann::json> samples;
    for (std::string line; std::getline(rows,line);) {
        auto row = nlohmann::json::parse(line);
        if (row.at("kind") == "model.prepared") samples.push_back(row.at("projection"));
    }
    check(samples.size() == 2 && samples[0].at("toolsSha256") == samples[1].at("toolsSha256") &&
        samples[0].at("headSha256") == samples[1].at("headSha256") && samples[1].at("commonMessages") == 1 &&
        samples[1].at("firstChangedRole") == "assistant" && samples[0].at("cutoff") < samples[1].at("cutoff"),
        "shadow comparison lost unchanged prefix, wire tools or evidence cutoff");
}

// 验证优先取件有界且接纳失败保留原消息。
// 步骤：填入两档队列 → 核对跨档配额 → 模拟接纳异常并重取。
void priority_mailboxes(const fs::path& run) {
    using namespace dfy::app;
    using namespace dfy::events;
    Application application({},run/"priority-plugins",run/"priority-user",dfy::model::Backend{
        [](const auto&,const auto&) -> dfy::model::Response { throw std::logic_error("priority fixture should not submit a model"); }});
    const auto network = std::make_shared<Mailboxes>(MailboxLimits{4,8,24,512});
    const auto owner = next_domain(), source = next_domain();
    const auto regular = network->open(owner,source), high = network->open(owner,source);
    MailboxIntake intake({network,owner},application.conversation());
    std::vector<MailboxId> seen;
    bool reject_once = false;
    const MailboxCategory categories[]{{"fixture.note",1}};
    const auto review = [&](const MailboxDelivery& item) -> std::optional<MailboxProjection> {
        if (reject_once) { reject_once = false; throw std::runtime_error("fixture admission failed"); }
        seen.push_back(item.mailbox);
        return {};
    };
    intake.watch(regular,categories,review);
    intake.watch(high,categories,review,MailboxPriority::high);
    const auto note = mailbox_text("fixture.note",1,"note",512);
    network->deliver_next(source,regular,note);
    for (unsigned i = 0; i < 4; ++i) network->deliver_next(source,high,note);
    for (unsigned i = 0; i < 5; ++i) check(intake.advance() == MailboxIntakeResult::handled,"mail was not handled");
    check(seen == std::vector{high,high,high,regular,high},"priority or fairness order differs");
    network->deliver_next(source,high,note);
    reject_once = true;
    check(intake.advance() == MailboxIntakeResult::rejected && network->front(owner,high) &&
        intake.advance() == MailboxIntakeResult::handled && !network->front(owner,high),"failed admission lost mail");
}

// 验证 ASK 的呈现、答案、时效和终态互相独立。
// 步骤：签发受限选项 → 拒绝抢答与跨身份答复 → 固定首答 → 推进超时和失败。
void ask_lifecycle() {
    using namespace dfy::app;
    auto now = dfy::agent::ToolTime{};
    AskService ask([&] { return now; });
    AskSpec spec{"Which wall?","player","game",{"river","house"},now+std::chrono::seconds(1)};
    const auto id = ask.open(4,spec);
    check(!ask.answer(id,4,"game","player","river") && ask.presented(id,4,"game",true) &&
        !ask.answer(id,5,"game","player","river") && !ask.answer(id,4,"game","other","river") &&
        !ask.answer(id,4,"game","player","roof") && ask.answer(id,4,"game","player","river"),"ask admitted invalid answer");
    check(ask.answer(id,4,"game","player","river") && !ask.answer(id,4,"game","player","house"),"ask overwrote answer");
    const auto pending = ask.open(4,spec), failure = ask.open(4,spec);
    check(ask.presented(failure,4,"game",false) && ask.status(failure)->phase == AskPhase::failed,"ask lost presentation failure");
    now += std::chrono::seconds(1);
    check(!ask.presented(pending,4,"game",true) && ask.status(pending)->phase == AskPhase::expired &&
        ask.status(id)->phase == AskPhase::answered,"ask deadline changed original terminal result");
}

// 验证原终态可在满箱后重试，且只由原信箱实际收件凭据结算。
// 步骤：填满原地址 → 重试同一正文 → 拒绝错绑回执 → 复用同一收件结果。
void reply_delivery() {
    using namespace dfy::app;
    using namespace dfy::events;
    Mailboxes boxes({2,1,4,1024});
    const auto inbox = boxes.open(3,2);
    boxes.deliver_next(2,inbox,mailbox_text("test.prior",1,"previous",1024));
    const AskReceipt original{{7,2},12,9,1,{}};
    ReplyDelivery delivery(3,original,inbox,"interaction.ask.result",R"({"answer":"river"})");
    delivery.advance(boxes,2,nullptr);
    check(boxes.read(3,inbox).event.get().id.sequence == 1 && !boxes.front(3,inbox),"full inbox displaced an original message");
    delivery.advance(boxes,2,nullptr);
    delivery.advance(boxes,2,nullptr);
    const auto message = boxes.read(3,inbox);
    check(message.event.get().id == EventId{inbox.value,2} && !boxes.front(3,inbox),"delivery was dropped or repeated");
    auto receipt = original;
    receipt.disposition = "matched";
    receipt.message = {inbox.value,1};
    check(!delivery.acknowledge(3,receipt,nullptr),"wrong message acknowledged");
    receipt.message = {inbox.value+1,2};
    check(!delivery.acknowledge(3,receipt,nullptr),"wrong inbox acknowledged");
    receipt.message = message.event.get().id;
    check(!delivery.acknowledge(4,receipt,nullptr) && delivery.acknowledge(3,receipt,nullptr) &&
        delivery.acknowledge(3,receipt,nullptr) && delivery.acknowledged(),"owner or duplicate acknowledgement was lost");
}

// 验证房间信箱与直接对话独立，自动和主动审查共用分类边界。
// 步骤：投递环境变化及被点名事件 → 验证唤醒模式 → 核对受信来源及模型请求。
void room_mailbox(const fs::path& run, bool automatic) {
    using namespace dfy::app;
    using namespace dfy::model;
    const auto user = run/"room-user";
    fs::create_directories(user);
    std::ofstream(user/"persona.json") << R"({"id":"room-test","revision":2,"instructions":"Speak like a teammate."})";
    Application application({},run/"room-plugins",user,Backend{[](const auto&,const Request& request) {
        check(request.messages.front().role == Role::system &&
            request.messages.front().content.find("邮件正文")!=std::string::npos &&
            request.messages.front().content.ends_with("【人格与风格 v1】\nSpeak like a teammate."),"room persona overwrote mailbox rules");
        return Response{{Role::assistant,dfy::test::business_messages(request).back().content}};
    }},ApplicationRunConfig{.wake_for_room_address = automatic,.trace_directory = run/"room-trace"});
    check(application.post_room_event(RoomEvent::presence_changed,"room-1","joined").state == dfy::events::DeliveryState::accepted,
        "presence fact did not enter its mailbox");
    application.advance_plugins();
    check(!application.conversation().busy(),"presence fact woke the main Agent");
    check(application.post_room_event(RoomEvent::addressed,"room-1","hello fish").state == dfy::events::DeliveryState::accepted,
        "room address did not queue independently");
    application.advance_plugins();
    application.advance_plugins();
    if (!automatic) {
        check(!application.conversation().busy(),"passive room route woke the Agent");
        check(application.review_room() == MailboxIntakeResult::handled && application.review_room() == MailboxIntakeResult::submitted,
            "review did not filter room presence before direct address");
    }
    check(application.conversation().busy() && application.conversation().status()->source == dfy::agent::RunOrigin::mailbox,
        "direct room address did not reach main Agent");
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(2);
    std::optional<CompletedTurn> result;
    while (application.conversation().busy() && std::chrono::steady_clock::now() < deadline) {
        if (auto completion = application.conversation().advance()) result = std::move(completion);
        std::this_thread::yield();
    }
    check(result && result->answer.find("room.addressed") != std::string::npos && result->answer.find("hello fish") != std::string::npos &&
        result->answer.find("joined") == std::string::npos,"room envelope lost its source or leaked a filtered fact");
    check(application.conversation().display().turns.empty(),"unsolicited mailbox round became a displayed dialogue");
    std::ostringstream observed;
    review_trace(application.trace_file(),dfy::agent::RunId{result->status.session,result->status.turn},observed);
    std::ostringstream replay;
    replay_trace(application.trace_file(),{result->status.session,result->status.turn},replay);
    check(replay.str().find("1 model requests, 0 tool exits, succeeded") != std::string::npos,
        "mailbox run did not replay from its own origin");
    check(observed.str().find("\"kind\":\"mail.dispatched\"") != std::string::npos &&
        observed.str().find("\"personaProfile\":\"room-test/2/") != std::string::npos &&
        observed.str().find("\"promptTemplate\":\"sha256:") != std::string::npos &&
        observed.str().find("room.addressed") != std::string::npos && observed.str().find("joined") == std::string::npos,
        "filtered room presence was incorrectly linked to a model run");
    if (!automatic) {
        application.post_room_event(RoomEvent::presence_changed,"room-1","filtered second");
        application.post_room_event(RoomEvent::addressed,"room-1","passive context");
        application.advance_plugins();
        check(!application.conversation().busy(),"passive mail started a turn before dialogue");
        const auto dialogue = application.submit_dialogue("direct text");
        check(dialogue.source == dfy::agent::RunOrigin::user_request,"combined turn lost its user origin");
        result.reset();
        const auto until = std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while (application.conversation().busy() && std::chrono::steady_clock::now() < until) {
            if (auto completion = application.conversation().advance()) result = std::move(completion);
            std::this_thread::yield();
        }
        check(result && result->answer.find("passive context") != std::string::npos &&
            result->answer.find("direct text") != std::string::npos &&
            result->answer.find("filtered second") == std::string::npos,"dialogue did not collect the reviewed mailbox context");
        const auto& turns = application.conversation().display().turns;
        check(turns.size() == 1 && turns.front().prompt == "direct text" && turns.front().run.sequence == dialogue.turn,
            "display replaced the visible prompt with an internal mailbox envelope");
        check(application.review_room() == MailboxIntakeResult::idle,"combined turn left a consumed message in the room inbox");
    }
}
}
// 在一个真实宿主进程中完成空插件问答、安装、加载、事件消费、卸载、再次问答和移除。
// 步骤：空插件建立上下文 → 安装加载并在模型等待时消费事件 → 验证失败隔离 → 卸载后继续上下文 → 移除并检查回收。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"observer package and workspace required"); const auto run = workspace(argv[2]);
        shared_application(run);
        display_outcomes(run);
        dialogue_queue(run);
        dialogue_inputs(run);
        builtin_room(run);
        trace_rewrites(run);
        priority_mailboxes(run);
        ask_lifecycle();
        reply_delivery();
        room_mailbox(run,true);
        room_mailbox(run,false);
        auto store = std::make_shared<dfy::events::EventStore>(64,64);
        auto channel = std::make_shared<dfy::events::BroadcastChannel>(store,4,8);
        PluginManager manager(run/"plugins",run/"user",channel,4);
        std::promise<void> release, entered; const auto gate = release.get_future().share(); auto started = entered.get_future();
        std::vector<dfy::model::Message> messages;
        dfy::agent::Session session({},channel,4,
        // 复制真实上下文并按测试回合选择等待、失败或成功，不接触宿主操作对象。
        // 步骤：保存输入 → 按提示等待或失败 → 返回完整回答。
        [&](const auto&,const auto& input) {
            messages = input;
            if (input.back().content == "with plugin") { entered.set_value(); gate.wait(); }
            if (input.back().content == "failure") throw std::runtime_error("model: integration failure");
            return "answer:"+input.back().content;
        });
        Release cleanup{release}; auto baseline = session.begin("baseline"); finish(session,manager,baseline);
        check(baseline->answer() == "answer:baseline" && manager.loaded().empty() && session.message_count() == 2,"empty-plugin answer failed");
        manager.install(argv[1]); check(manager.loaded().empty() && !manager.load("example.observer"),"install or explicit load failed");
        auto request = session.begin("with plugin"); started.wait();
        for (unsigned i = 0; i < 8; ++i) manager.advance();
        check(request->operation()->state() == OperationState::pending && statistics(manager).events == 1,"model wait blocked actual DLL event consumption");
        cleanup.open(); finish(session,manager,request);
        check(request->answer() == "answer:with plugin" && statistics(manager).events == 2 && !statistics(manager).failures &&
            messages.size() == 3 && messages[0].content == "baseline" && session.message_count() == 4,"loaded observer changed model context or result");
        auto failure = session.begin("failure"); finish(session,manager,failure);
        check(failure->operation()->state() == OperationState::failed && session.message_count() == 4 && statistics(manager).events == 4,
            "model failure polluted context or stopped observer");
        check(manager.unload("example.observer") == ShutdownResult::closed && manager.loaded().empty(),"integration observer did not unload");
        baseline.reset(); request.reset(); failure.reset(); auto after = session.begin("after unload"); finish(session,manager,after);
        check(after->answer() == "answer:after unload" && messages.size() == 5 && messages[4].content == "after unload" && session.message_count() == 6,
            "unloaded plugin became a model dependency");
        const auto removed = manager.remove("example.observer");
        check(removed.files.residual.empty() && manager.catalog()->packages.empty() && !store->references() && !channel->size(),"integration removal leaked package or event references");
        session.close(); check(session.closed() && manager.close() && !PluginManager::retained() && PluginInstance::retained().empty(),"integration left retained cleanup");
        std::cout << "Application lifecycle checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
