#include "application.hpp"
#include "task_memory_fixture.hpp"
#include "main_messages.hpp"
#include <dafeiyu/model_protocol.hpp>
#include <dafeiyu/digest.hpp>
#include <dafeiyu/dynamic_library.hpp>
#include <dafeiyu/tool_abi_v2.h>
#include <fstream>
#include <iostream>
#include <thread>

namespace dfy::tests::task_memory {
using namespace dfy;
using Json = nlohmann::json;
namespace fs = std::filesystem;
// 把失败定位到当前语料中的断言。
void require(bool ok, const std::string& reason) { if (!ok) throw std::runtime_error(reason); }
// 推进到真实退出，审批与取消均沿原宿主入口执行。
void drain(app::Application& host, const Json& row) {
    const auto until = std::chrono::steady_clock::now()+std::chrono::seconds(6);
    while (host.conversation().busy()) {
        host.advance_plugins();
        host.conversation().advance();
        for (const auto& approval : host.confirmations()) if (approval.state==agent::ConfirmationState::waiting) {
            host.decide(approval.id,approval.revision,row.value("approve",true));
            if (row.value("cancelApproved",false)) host.conversation().cancel();
        }
        if (row.value("cancelApplied",false) && std::ranges::any_of(host.conversation().diagnostic()->timeline.nodes,
            [](const auto& item){return item.effect==agent::ToolEffect::applied;})) host.conversation().cancel();
        if (row.value("cancelPlan",false)) {
            const auto plans = host.conversation().diagnostic()->plans;
            if (!plans.empty() && !plans.back().steps.empty()) host.conversation().cancel();
        }
        require(std::chrono::steady_clock::now()<until,"task did not exit");
        std::this_thread::yield();
    }
}
// 从 Trace 读取独立对照的原工具回执，并逐份核对实际进入后端的请求摘要。
std::vector<Json> trace(const fs::path& path, const std::vector<model::Request>& requests) {
    std::ifstream input(path,std::ios::binary);
    std::vector<Json> works;
    std::size_t seen = 0;
    for (std::string line;std::getline(input,line);) {
        const auto event = Json::parse(line);
        if (event.at("kind")=="tool.exited" && event.at("source")=="main") works.push_back(event);
        if (event.at("kind")=="model.prepared") {
            const auto wire = model::encode_request_fields(requests.at(seen++),event.at("thinking").get<bool>());
            require(event.at("wireSha256")==platform::sha256(wire),"wire differs from prepared request");
        }
    }
    require(seen==requests.size(),"missing prepared request");
    return works;
}

// 通过本体的真实任务入口运行固定提议，宿主及插件提供工作、效果与独立读回。
std::vector<Json> perform(const Json& row, const fs::path& data, const fs::path& package, const std::string& goal,
    std::size_t& request_count) {
    const auto id = row.at("id").get<std::string>(), tool = row.at("tool").get<std::string>();
    const auto plugin = tool.substr(0,tool.find('/')), reader = plugin+"/read";
    std::vector<model::Request> requests;
    bool warmup = row.value("warmup",false);
    std::size_t start = 0;
    app::ApplicationRunConfig config;
    config.trace_directory = data/"logs";
    app::Application host({},data/id,data,model::Backend{[&](const auto&,const model::Request& request)->model::Response {
        const auto index = requests.size()-start;
        requests.push_back(request);
        const auto& script = row.at("script");
        const auto step = warmup ? (index==0 ? "change" : "claim") : index<script.size() ? script[index].get<std::string>() : "claim";
        if (step=="plan") {
            Json plan{{"schema_version",1},{"steps",Json::array({{{"id","work"},{"objective","施行原计划"},{"completion_criterion","读回目标状态"},{"tools",{reader,tool}}}})}};
            return {{model::Role::assistant,plan.dump()}};
        }
        if (step=="claim") return {{model::Role::assistant,"我已经完成，目标肯定达成。"}};
        const auto suffix = step=="read" ? "_read" : "_"+tool.substr(tool.find('/')+1);
        const auto found = std::ranges::find_if(request.tools,[&](const auto& item){return item.name.ends_with(suffix);});
        require(found!=request.tools.end(),"required tool missing");
        return {{model::Role::assistant,"",{{"actual-"+std::to_string(index),found->name,row.value("args",std::string{"{}"})}}},model::FinishReason::tool_calls};
    }},config);
    host.bind_messages(std::make_shared<app::MessageStore>(data),id,nullptr);
    const auto installed = host.plugins().install(package);
    std::optional<platform::DynamicLibrary> fixture;
    if (row.value("unknown",false)) {
        fixture.emplace(installed.entry);
        reinterpret_cast<void (DFY_V2_CALL*)(uint32_t)>(fixture->symbol("dfy_tool_test_mode"))(7);
    }
    require(host.plugins().load(plugin)==0,"native fixture did not load");
    if (warmup) {
        host.submit_confirmed(tool,"准备不同的初始读数");
        drain(host); start=requests.size(); warmup=false;
    }
    if (row.contains("expected")) host.submit_verified(reader,tool,row.at("expected"),goal);
    else if (row.value("readOnly",false)) host.submit_read_only(tool,goal);
    else host.submit_confirmed(tool,goal);
    drain(host,row);
    request_count += requests.size();
    return trace(host.trace_file(),requests);
}

// 重新打开档案，通过私聊读原件，比较动作、回执及实收答复，并验证空闲推进。
void recall(const Json& row, const fs::path& data, const std::string& goal, const std::vector<Json>& works,
    unsigned& reads, std::size_t& request_count) {
    const auto id = row.at("id").get<std::string>();
    const auto db = std::make_shared<app::MessageStore>(data);
    std::vector<model::Request> requests;
    std::string selector, base, delivery;
    std::size_t offset = 0;
    bool first = true, room = false;
    Json selection;
    app::ApplicationRunConfig config;
    config.trace_directory = data/"logs";
    app::Application host({},data/"empty",data,model::Backend{[&](const auto&,const model::Request& request)->model::Response {
        requests.push_back(request);
        if (room) {
            const auto wire = model::encode_request_fields(request,false);
            require(wire.find(base)==wire.npos && wire.find(id)==wire.npos,"private task appeared in room");
            return {{model::Role::assistant,"收到。"}};
        }
        if (first) {
            first = false;
            Json args{{"topics",{selector}},{"view","tasks"},{"count",1},{"need","single"},{"offset",offset}};
            return {{model::Role::assistant,"",{{"lookup","memory_search",args.dump()}}},model::FinishReason::tool_calls};
        }
        const auto tail = Json::parse(request.messages.back().content);
        for (const auto& sample : tail.at("items")) if (sample.at("key")=="memory.tasks") {
            selection = sample.at("value").at("items");
            require(selection.dump().size()<=2048,"unbounded projection");
            delivery = "没有可用依据。";
            if (!selection.empty()) {
                const auto& hit = selection.front();
                delivery = "依据 "+hit.at("reference").get<std::string>();
                for (const auto key : {"execution","effect","verification","actionRef","receiptRef","readbackRef","review"})
                    if (hit.contains(key)) delivery += " "+hit.at(key).get<std::string>();
            }
            return {{model::Role::assistant,delivery}};
        }
        throw std::runtime_error("missing private task projection");
    }},config);
    host.bind_messages(db,"recall-"+id,nullptr);
    // 每个查询都走实际聊天和工具续行，检查收到的正是这一轮的引证。
    const auto query = [&](const std::string& term, std::size_t at=0) {
        selector = term; offset = at; first = true; selection = nullptr;
        host.main_messages()->terminal("lookup-"+std::to_string(++reads),"请查本体的任务记录。");
        drain(host);
        require(selection.is_array(),"task lookup failed");
        require(db->page(true,0,db->watermark(),1).records.front().envelope.at("text")==delivery,"citation not delivered");
        return selection.empty() ? Json(nullptr) : selection.front();
    };
    const auto initial = query(id);
    require(!initial.is_null(),"lost task after restart");
    base = initial.at("reference");
    require(initial.at("verification")==row.at("verification") && initial.at("execution")==row.at("execution") &&
        initial.value("effect",std::string{})==row.value("effect",std::string{}) && initial.at("count")==works.size() &&
        works.size()==row.at("count").get<unsigned>(),"intent, dispatch, receipt or verification differs: "+initial.dump());
    require(initial.contains("plan")==row.value("planned",false) && !initial.contains("review") &&
        initial.at("reviewState")==(row.contains("reason") ? "queued" : "facts_only"),"incorrect plan or premature review");
    if (row.contains("reason")) require(initial.at("reviewReason")==row.at("reason"),"incorrect idle trigger");
    require(initial.contains("readbackRef")==(row.at("verification")!="unconfirmed"),"invented readback");
    if (row.contains("runResult")) require(initial.at("runResult")==row.at("runResult"),"wrong task outcome");
    std::string action = initial.value("firstAction",std::string{});
    for (const auto& fact : works) {
        const auto proposed = query(action);
        if (row.contains("args")) require(proposed.at("payload")==row.at("args"),"lost original arguments");
        require(proposed.at("payload")==fact.at("arguments") && proposed.at("work").at("callId")==fact.at("callId") &&
            proposed.at("work").at("candidate")==fact.at("candidate") && proposed.at("work").at("step")==fact.at("step"),"different original proposal");
        const auto original = query(proposed.at("receiptRef"));
        for (const auto key : {"accepted","dispatched","succeeded","invocation","candidate","fault","error"})
            require(original.at("work").at(key)==fact.at(key),"different original work receipt");
        require(original.at("payload")==fact.at("output") && original.at("effect")==fact.at("effect") &&
            original.at("result")==fact.at("result") && original.at("actionRef")==action,"receipt refers to another action");
        action = proposed.value("nextAction",std::string{});
    }
    require(action.empty(),"fabricated extra proposal");
    if (initial.contains("readbackRef")) {
        require(query(initial.at("baselineRef")).at("payload")==row.at("before") &&
            query(initial.at("readbackRef")).at("payload")==row.at("after"),"wrong baseline or cross-run readback");
    }
    if (row.value("long",false)) {
        require(initial.at("goal").get<std::string>().ends_with("…"),"shortened goal looks complete");
        std::string reconstructed;
        do {
            const auto piece = query(base+"/goal",reconstructed.size());
            reconstructed += piece.at("payload").get<std::string>();
            if (!piece.contains("nextOffset")) break;
            require(piece.at("nextOffset")==reconstructed.size(),"wrong UTF-8 continuation");
        } while (true);
        require(reconstructed==goal,"long original lost");
    }
    if (row.value("room",false)) {
        room = true;
        host.post_room_event(app::RoomEvent::addressed,"书房","有人到了。");
        host.advance_plugins();
        require(host.conversation().busy(),"pending foreground mail did not win");
        drain(host); room = false;
        require(query(base).at("reviewState")==initial.at("reviewState"),"review ran before foreground mail");
    }
    if (row.contains("backlog")) for (const auto& pending : row.at("backlog"))
        require(query(pending.at("id")).at("reviewState")=="queued","earlier task ran while foreground was busy");
    const auto before_idle = requests.size();
    host.advance_plugins();
    require(requests.size()==before_idle && !host.conversation().busy(),"idle work started a model run");
    const auto reviewed = query(base);
    require(reviewed.at("reviewState")==(row.contains("reason") ? "reviewed" : "facts_only") &&
        reviewed.at("verification")==initial.at("verification"),"idle review changed a fact");
    if (row.contains("reason")) {
        require(reviewed.at("review").get<std::string>().find(row.at("lesson").get<std::string>())!=std::string::npos &&
            reviewed.at("receiptRef")==initial.at("receiptRef"),"idle lesson lost source or uncertainty");
    }
    if (row.contains("backlog")) {
        const auto oldest = row.at("backlog").at(0).at("id").get<std::string>();
        const auto second = row.at("backlog").at(1).at("id").get<std::string>();
        require(query(oldest).at("reviewState")=="queued" && query(second).at("reviewState")=="queued","idle processed more than one task");
        host.advance_plugins();
        require(query(second).at("reviewState")=="reviewed","unverified effect lost priority to a routine transition");
        const auto pending = query(oldest);
        require(pending.at("reviewState")=="queued","oldest item bypassed priority");
        host.main_messages()->change_memory(pending.at("reference").get<std::string>(),std::nullopt);
        host.advance_plugins();
        require(query(oldest).is_null() && query(pending.at("receiptRef")).is_null(),"revoked queued task was reviewed");
        require(query("00000000000000000000000000000000/task/1").is_null(),"foreign task leaked");
    }
    host.advance_plugins();
    require(query(base)==reviewed,"idle replay changed review");
    if (row.value("revoke",false)) {
        host.main_messages()->change_memory(base,std::nullopt);
        require(query(base).is_null() && query(initial.at("receiptRef")).is_null(),"revoked original still readable");
    }
    trace(host.trace_file(),requests);
    request_count += requests.size();
}
}

// 读取跨领域的通用场景，以两种实际插件验证本体工具任务的原始行动及回执。
int wmain(int argc, wchar_t** argv) {
    using namespace dfy::tests::task_memory;
    try {
        if (argc!=5 && argc!=6) return 2;
        const fs::path root(argv[4]);
        fs::create_directories(root);
        fs::path data;
        for (unsigned n=1;;++n) if (fs::create_directory(data=root/std::to_string(n))) break;
        if (argc==6 && std::wstring_view(argv[5])==L"--understandings") {
            understanding_cases(argv[3],data,argv[1],argv[2]); return 0;
        }
        if (argc==6 && std::wstring_view(argv[5])==L"--recall") {
            understanding_recall(argv[3],data); return 0;
        }
        std::ifstream corpus(fs::path(argv[3]),std::ios::binary);
        unsigned cases=0, reads=0;
        std::size_t requests=0;
        for (std::string line;std::getline(corpus,line);) {
            const auto row = Json::parse(line);
            const auto id = row.at("id").get<std::string>();
            try {
                auto goal = id+" "+row.at("goal").get<std::string>();
                if (row.value("long",false)) for (int i=0;i<180;++i) goal += "图书馆";
                const auto profile = row.contains("backlog") ? data/"backlog" : data;
                if (row.contains("backlog")) for (const auto& pending : row.at("backlog")) {
                    perform(pending,profile,pending.at("tool").get<std::string>().starts_with("test.tools") ? argv[2] : argv[1],
                        pending.at("id").get<std::string>()+" "+pending.at("goal").get<std::string>(),requests);
                }
                const auto works = perform(row,profile,row.at("tool").get<std::string>().starts_with("test.tools") ? argv[2] : argv[1],goal,requests);
                recall(row,profile,goal,works,reads,requests);
            } catch (const std::exception& error) { throw std::runtime_error(id+": "+error.what()); }
            ++cases;
        }
        require(cases!=0,"empty task corpus");
        std::cout<<cases<<" task cases, "<<reads<<" private recalls, "<<requests<<" verified backend requests\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
