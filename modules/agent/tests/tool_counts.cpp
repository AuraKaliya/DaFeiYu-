#include <dafeiyu/session.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>
#include <thread>

namespace {
using namespace dfy::agent;
using namespace dfy::model;
using Json = nlohmann::json;

// 从有权限的正式会话读回指定决策的原计数采样，显式尾注保持当步的实际内容。
Json read_counts(const Session& session, const Json& tail) {
    const auto epoch = std::stoull(tail.at("scope").at("epoch").get<std::string>());
    const auto snapshot = session.read_status(epoch,tail.at("snapshot").get<std::string>());
    const auto items = Json::parse(snapshot).at("status").at("items");
    const auto sample = std::ranges::find_if(items,[](const auto& row) { return row.at("key") == "host.tool_counts"; });
    if (sample == items.end() || sample->at("attention").at("mode") != "implicit" || !tail.at("items").empty())
        throw std::runtime_error("missing implicit tool count");
    return sample->at("value");
}

// 在两个读工具接纳后由第一个撤去第二个的授权，用不同回合和新绑定校验三个统计范围。
// 步骤：提出三项并拒收坏参数 → 接纳两项且仅实际派发一项 → 续行再读 → 验证跨回合与换绑。
void verify() {
    ToolCatalog catalog;
    const std::string schema = R"({"type":"object","properties":{},"additionalProperties":false})";
    unsigned reads = 0, late_dispatches = 0, replies = 0;
    const auto late = catalog.add({"test/late",1,1,{"late","后续读取",schema}},[&](const auto&) {
        ++late_dispatches; return completed_tool({true,ToolEffect::not_applied,"late",ToolError::none});
    });
    catalog.add({"test/read",1,1,{"read","读取",schema}},[&](const auto&) {
        ++reads; catalog.enable(late,false); return completed_tool({true,ToolEffect::not_applied,"read",ToolError::none});
    });
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,64),1,8);
    Session session({},channel,Backend{[&](const Config&,const Request&) -> Response {
        if (++replies==1) return {{Role::assistant,"",{{"one","read","{}"},{"two","late","{}"},{"three","read",R"({"bad":1})"}}},
            FinishReason::tool_calls};
        if (replies==2) return {{Role::assistant,"",{{"next","read","{}"}}},FinishReason::tool_calls};
        return {{Role::assistant,"结束"}};
    }});
    RunOptions options;
    const std::vector<std::string> ids{"test/read","test/late"};
    options.tools = catalog.snapshot(ids,false);
    options.decision_status = DecisionStatusOptions{"first",{},false,{},true};
    std::vector<Json> tails;
    options.model_capture = [&](const ModelCapture& row) {
        if (row.phase == ModelCapturePhase::prepared) tails.push_back(Json::parse(row.input->messages.back().content));
    };
    // 只推进原会话，所有断言都依据已退出回合和正式请求。
    const auto run = [&] {
        auto request = session.begin("核对工具计数",options);
        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(4);
        while (session.busy() && std::chrono::steady_clock::now()<deadline) { session.advance(); std::this_thread::yield(); }
        if (session.busy() || request->answer()!="结束") throw std::runtime_error("counted turn did not exit");
        return request;
    };
    const auto turn = run();
    const Json zero{{"proposed",0},{"admitted",0},{"dispatched",0}}, first{{"proposed",3},{"admitted",2},{"dispatched",1}},
        second{{"proposed",1},{"admitted",1},{"dispatched",1}}, total{{"proposed",4},{"admitted",3},{"dispatched",2}};
    const auto initial = read_counts(session,tails.at(0)), one = read_counts(session,tails.at(1)), two = read_counts(session,tails.at(2));
    if (reads!=2 || late_dispatches || tails.size()!=3 || initial.contains("response") || initial.at("session")!=zero ||
        one.at("session")!=first || one.at("turn")!=first || one.at("response")!=first || one.at("responseStep")!=1 ||
        two.at("turn")!=total || two.at("response")!=second || two.at("responseStep")!=2 ||
        turn->tool_counts().admitted!=3) throw std::runtime_error("proposal, admission or dispatch scope was conflated");
    run();
    const auto next = read_counts(session,tails.back());
    if (next.at("session")!=total || next.at("turn")!=zero || next.contains("response") || read_counts(session,tails[1])!=one)
        throw std::runtime_error("new turn counted an old response twice or changed its snapshot");
    options.decision_status->binding = "rebound";
    run();
    if (read_counts(session,tails.back()).at("session")!=zero) throw std::runtime_error("new binding inherited old tool counts");
}
}
// 用固定响应、真实宿主授权及执行器运行计数回归。
int main() {
    try { verify(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
