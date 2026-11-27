#include "task_memory_fixture.hpp"
#include "main_messages.hpp"
#include <dafeiyu/model_protocol.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <numeric>

namespace dfy::tests::task_memory {
namespace {
// 从原任务语料选取已经验收的固定脚本。
Json fixture(const fs::path& corpus, std::string_view id) {
    std::ifstream input(corpus,std::ios::binary);
    for (std::string line; std::getline(input,line);) {
        auto row = Json::parse(line);
        if (row.at("id").get_ref<const std::string&>()==id) return row;
    }
    throw std::runtime_error("missing resume fixture");
}

// 结算 T03 和另一档案中的同序号任务，以原 Trace 固定参照回执。
// 步骤：运行原夹具并处理空闲复盘 → 读档案身份 → 写出与实际回执对应的预期。
void seed_resume(const fs::path& corpus, const fs::path& root, const fs::path& panel) {
    const auto row = fixture(corpus,"T03");
    const auto goal = row.at("goal").get<std::string>();
    std::size_t requests = 0;
    const auto works = perform(row,root/"seed",panel,goal,requests,true);
    perform(row,root/"foreign-origin",panel,goal,requests,true);
    require(works.size()==3 && works.at(2).at("output")==row.at("after"),"T03 original readback differs");

    const app::MessageStore source(root/"seed"), foreign(root/"foreign-origin");
    require(source.conversation()!=foreign.conversation(),"fixture archives share an identity");
    const auto reference = source.conversation()+"/task/1";
    const auto action = std::to_string(works.at(1).at("candidate").get<std::uint64_t>());
    const auto readback = std::to_string(works.at(2).at("candidate").get<std::uint64_t>());
    std::ofstream proof(root/"task.json",std::ios::binary);
    proof << Json{{"reference",reference},{"foreignReference",foreign.conversation()+"/task/1"},{"goal",goal},
        {"expected",row.at("expected")},{"effect",row.at("effect")},{"verification",row.at("verification")},
        {"actionRef",reference+"/action/"+action},{"receiptRef",reference+"/receipt/"+action},
        {"readbackRef",reference+"/receipt/"+readback},{"after",row.at("after")}}.dump();
    require(proof.good(),"could not write task evidence for replay");
}

// 从替身实收的决策尾注取一份任务概览，同时核对证据预算。
Json received_task(const model::Request& request) {
    if (request.messages.empty() || request.messages.back().source!=model::MessageSource::host_status) return nullptr;
    const auto tail = Json::parse(request.messages.back().content);
    for (const auto& sample : tail.at("items")) if (sample.at("key")=="memory.tasks") {
        const auto& items = sample.at("value").at("items");
        require(items.size()<=1 && items.dump().size()<=2048,"resume exceeded the evidence budget");
        if (!items.empty()) return items.front();
    }
    return nullptr;
}

// 检查房间消息确实进入本轮，并且当前工具与正文均服从实际受众。
void check_room(const model::Request& request) {
    const auto room = std::ranges::find_if(request.messages,[](const auto& message) {
        return message.source==model::MessageSource::mailbox &&
            Json::parse(message.content).value("category",std::string{})=="room.addressed";
    });
    require(room!=request.messages.end(),"room mail did not reach the model");
    require(std::ranges::none_of(request.tools,[](const auto& tool) { return tool.name=="memory_search"; }),
        "memory lookup exposed to the room audience");
}
}

// 重开同一快照，用同一个替身比较首次得到依据的请求并核对实际投递。
// 步骤：准备对照条件 → 经原私聊入口接纳接续 → 查实收请求、原件与投递 → 输出测量。
void task_resume(const fs::path& corpus, const fs::path& root, const fs::path& panel, const std::string& phase) {
    if (phase=="seed") { seed_resume(corpus,root,panel); return; }
    const auto data = root/phase;
    require(fs::is_regular_file(data/"main-conversation.sqlite3"),"missing replay snapshot");
    std::ifstream proof(root/"task.json",std::ios::binary);
    const auto expected = Json::parse(proof);
    const auto goal = expected.at("goal").get<std::string>();
    const auto reference = expected.at("reference").get<std::string>();
    const bool baseline = phase=="baseline", same_name = phase=="same-name", room = phase=="room";
    const bool positive = baseline || phase=="first" || same_name;
    if (same_name) {
        std::size_t requests = 0;
        const auto second = perform(fixture(corpus,"T10"),data,panel,goal,requests);
        require(second.size()==3 && second.at(2).at("output")=="idle","same-name task was not settled");
    }

    const auto db = std::make_shared<app::MessageStore>(data);
    require(reference.starts_with(db->conversation()+"/task/"),"replay opened the wrong archive");
    std::vector<model::Request> requests;
    std::vector<std::size_t> bytes;
    Json evidence;
    std::size_t first_evidence = 0;
    std::string answer;
    app::ApplicationRunConfig options;
    options.trace_directory = data/"resume-log";
    app::Application host({},data/"empty",data,model::Backend{[&](const auto& config,const model::Request& request)->model::Response {
        requests.push_back(request);
        bytes.push_back(model::encode_request_fields(request,config.thinking_enabled()).size());
        if (auto item=received_task(request); !item.is_null()) {
            evidence = std::move(item);
            first_evidence = requests.size();
            answer = "依据 "+evidence.at("reference").get<std::string>()+"。"+evidence.value("review",std::string{})+"继续前先读面板当前状态。";
            return {{model::Role::assistant,answer}};
        }
        const auto names_goal = std::ranges::any_of(request.messages,[&](const auto& message) {
            return message.source==model::MessageSource::human && message.content.find(goal)!=std::string::npos;
        });
        const auto can_search = std::ranges::any_of(request.tools,[](const auto& tool) {return tool.name=="memory_search";});
        if (requests.size()==1 && names_goal && can_search) {
            const Json args{{"topics",{goal}},{"view","tasks"},{"need","single"},{"count",1}};
            return {{model::Role::assistant,"",{{"lookup","memory_search",args.dump()}}},model::FinishReason::tool_calls};
        }
        answer = "请确认要接续的任务。";
        return {{model::Role::assistant,answer}};
    }},options);
    host.bind_messages(db,"resume-"+phase,nullptr);
    if (phase=="revoked") host.main_messages()->change_memory(reference,std::nullopt);
    if (room) require(host.post_room_event(app::RoomEvent::addressed,"书房","有人到了。").state==events::DeliveryState::accepted,
        "room mail did not enter its mailbox");
    const auto selected = baseline ? std::string{} : phase=="foreign" ? expected.at("foreignReference").get<std::string>() : reference;
    const auto prompt = positive ? "接着处理「"+goal+"」，先告诉我上次做到哪了。" : "接着处理所选任务，先告诉我上次做到哪了。";
    host.main_messages()->terminal("resume",prompt,selected);
    drain(host);

    const auto works = trace(host.trace_file(),requests);
    require(works.size()==(baseline ? 1u : 0u) && (!baseline || works.front().at("toolId")=="main/memory_search"),
        "unexpected tool use during resume");
    require(requests.size()==(baseline ? 2u : 1u),"unexpected model request count");
    const auto input = db->find("terminal","resume");
    const auto delivered = db->page(true,0,db->watermark(),1).records.front();
    require(input && delivered.envelope.at("replyTo")==std::to_string(input->id) && delivered.envelope.at("text")==answer,
        "resume answer was not delivered to its original caller");
    if (room) check_room(requests.front());

    if (positive) {
        require(first_evidence==(baseline ? 2u : 1u),"correct evidence arrived on the wrong request");
        for (const auto key : {"reference","goal","expected","effect","verification","actionRef","receiptRef","readbackRef"})
            require(evidence.at(key)==expected.at(key),std::string("mismatched T03 evidence: ")+key);
        require(evidence.at("execution")=="dispatched" &&
            evidence.at("review").get<std::string>().find("动作后读回「"+expected.at("after").get<std::string>()+"」")!=std::string::npos,
            "T03 review does not reflect the original readback");
        if (baseline) {
            std::ofstream projection(root/"baseline.json",std::ios::binary);
            projection << evidence.dump();
            require(projection.good(),"could not save the baseline evidence");
        } else {
            std::ifstream projection(root/"baseline.json",std::ios::binary);
            require(Json::parse(projection)==evidence,"resume changed the baseline evidence");
        }
    } else {
        const auto wire = model::encode_request_fields(requests.front(),false);
        require(evidence.is_null() && wire.find(reference)==wire.npos && wire.find(selected)==wire.npos && wire.find(goal)==wire.npos,
            "unavailable task evidence reached the model");
    }
    std::cout << Json{{"phase",phase},{"firstEvidenceRequest",first_evidence},{"verification",evidence.is_null() ? Json(nullptr) : evidence.at("verification")},
        {"wireBytes",bytes},{"totalWireBytes",std::accumulate(bytes.begin(),bytes.end(),std::size_t{})},
        {"reference",evidence.is_null() ? Json(nullptr) : evidence.at("reference")},
        {"readbackRef",evidence.is_null() ? Json(nullptr) : evidence.at("readbackRef")}}.dump() << '\n';
}
}
