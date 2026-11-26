#include "run_log.hpp"
#include <dafeiyu/session.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace dfy;
using namespace agent;
using namespace model;
using Json = nlohmann::json;
// 以原始回合报告契约轨迹或实际执行的差异。
void check(bool condition, const char* detail) { if (!condition) throw std::runtime_error(detail); }

// 同会话连续改变展示次序、执行授权、契约版本和历史代次，始终走正式模型提交。
// 步骤：冻结原序与收窄授权 → 在提交前撤权 → 恢复并升级契约 → 核对每步的原定义位置。
void verify(const std::filesystem::path& root) {
    auto log = std::make_shared<app::RunLog>(root);
    auto events = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(16,64),1,8);
    ToolCatalog catalog;
    const std::string id = "fixture/read", other = "fixture/alternate";
    const std::string schema = R"({"type":"object","properties":{},"additionalProperties":false})";
    unsigned invoked = 0;
    const auto factory = [&](const ToolInvocation&) {
        ++invoked;
        return completed_tool({true,ToolEffect::not_applied,"read",ToolError::none});
    };
    const auto owner = runtime::next_domain();
    auto handle = catalog.add({id,owner,1,{"read","original contract",schema}},factory);
    catalog.add({other,owner,1,{"alternate","alternate contract",schema}},factory);
    Session session({},events,Backend{[](const Config&,const Request& input) {
        if (input.messages.back().content=="revoke after preparation")
            return Response{{Role::assistant,"",{{"pending","read","{}"}}},FinishReason::tool_calls};
        return Response{{Role::assistant,"received"}};
    }},12,log->sinks("fixture"));
    const std::vector<std::string> both{id,other}, reversed{other,id};
    // 将每个测试输入绑定到当步授权，等待工作与回合均实际退出。
    const auto run = [&](const std::vector<std::string>& grant, const std::string& prompt, bool clear=false) {
        RunOptions options;
        options.tools = catalog.snapshot(grant);
        options.replace_history = clear;
        bool revoked = false;
        options.text_hook = [&](TextHookPoint point,const auto&,auto field,auto,auto) -> std::optional<std::string> {
            if (prompt=="revoke after preparation" && !revoked && point==TextHookPoint::model_request && field=="system") {
                catalog.enable(handle,false);
                revoked = true;
            }
            return {};
        };
        const auto turn = session.begin(prompt,std::move(options));
        const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(4);
        while (session.busy() && std::chrono::steady_clock::now()<end) { session.advance(); std::this_thread::yield(); }
        check(!session.busy() && turn->answer()=="received","turn failed before recording its actual input");
    };
    run(both,"introduce");
    run(reversed,"repeat");
    run(both,"revoke after preparation");
    check(!invoked,"previous contract authorized a revoked tool");
    catalog.enable(handle,true);
    run(reversed,"back in scope");
    catalog.remove(handle);
    handle = catalog.add({id,owner,2,{"read","revised contract",schema}},factory);
    run(both,"updated");
    run(both,"new history",true);

    std::vector<Json> rows;
    std::ifstream stream(log->path());
    for (std::string line; std::getline(stream,line);) {
        auto row = Json::parse(line);
        if (row.at("kind")=="model.prepared") rows.push_back(std::move(row));
    }
    check(rows.size()==7,"unexpected decision count");
    const auto at = [&](std::size_t n) -> const Json& { return rows.at(n).at("toolContracts"); };
    const auto change = [&](std::size_t n, std::string state) {
        return at(n).at("changes")==Json::array({Json{{"id",id},{"state",std::move(state)}}});
    };
    check(at(0).at("changes").size()==2 && at(0).at("available").at(0).at("id")==other &&
        at(1).at("changes").empty() && at(1).at("available")==at(0).at("available") &&
        at(2).at("changes").empty() && change(3,"left") && change(4,"entered") && change(5,"updated"),
        "introduced, reordered, temporarily unavailable or revised contract was misattributed");
    const auto& origin = at(4).at("available").at(1).at("origin");
    const auto& updated = at(5).at("available").at(1);
    check(origin.at("run")==1 && origin.at("step")==1 && origin.at("attempt")==1 &&
        rows[0].at("input").at("tools").at(origin.at("toolIndex").get<std::size_t>()).at("description")=="original contract" &&
        updated.at("contract")==2 && updated.at("origin").at("run")==5 && at(6).at("baseline")=="history" &&
        at(6).at("epoch")!=at(5).at("epoch") && at(6).at("available").at(1).at("origin").at("run")==6,
        "contract reference did not resolve to its own version and history");
}
}
// 使用本地固定响应和隔离的轨迹目录验收工具契约。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc!=2) return 2;
        verify(argv[1]);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
