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
// 步骤：装载夹具并接纳任务 → 裁定审批及等待退出 → 按需处理空闲复盘并核对原回执。
std::vector<Json> perform(const Json& row, const fs::path& data, const fs::path& package, const std::string& goal,
    std::size_t& request_count, bool review) {
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
    if (row.contains("case")) {
        const app::ComparisonTag tag{row.at("case"),row.at("checkpoint")};
        const auto accepted=host.submit_verified_once(1,reader,tool,row.at("expected"),goal,tag);
        const auto replay=host.submit_verified_once(1,reader,tool,row.at("expected"),goal,tag);
        require(!accepted.reused && replay.reused && accepted.status.turn==replay.status.turn,"comparison task replayed");
        bool denied=false; try {host.submit_verified_once(1,reader,tool,row.at("expected"),goal,app::ComparisonTag{tag.name,"different checkpoint"});}
        catch (const std::invalid_argument&) {denied=true;}
        require(denied,"numbered task changed scope");
    } else if (row.contains("expected")) host.submit_verified(reader,tool,row.at("expected"),goal);
    else if (row.value("readOnly",false)) host.submit_read_only(tool,goal);
    else host.submit_confirmed(tool,goal);
    drain(host,row);
    if (review) host.advance_plugins();
    request_count += requests.size();
    return trace(host.trace_file(),requests);
}

}
