#include "application.hpp"
#include "main_messages.hpp"
#include <dafeiyu/digest.hpp>
#include <dafeiyu/model_protocol.hpp>
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <thread>

namespace {
using namespace dfy;
using Json = nlohmann::json;
using Sources = std::map<std::string,app::StoredMessage>;

// 报告与语料编号关联的断言错误。
void require(bool ok, const std::string& reason) { if (!ok) throw std::runtime_error(reason); }

// 让业务线程推进到本回合成功结算。
void drain(app::Application& host) {
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while (host.conversation().busy()) {
        host.advance_plugins();
        host.conversation().advance();
        require(std::chrono::steady_clock::now()<end,"dialogue timed out");
        std::this_thread::yield();
    }
    require(host.conversation().status()->result==runtime::OperationState::succeeded,"turn failed");
}

// 逐项核对选中原件、顺序、字节和来源，同时在完整送模编码中排除未选原话。
void check_request(const model::Request& request, const Sources& sources, const std::string& profile,
    const Json& row, unsigned step, bool followup) {
    const bool owner = !row.contains("audience");
    for (const std::string name : {"memory_note","memory_search"}) {
        const auto tool = std::ranges::find(request.tools,name,&model::ToolDefinition::name);
        require((tool!=request.tools.end())==owner,"incorrect tool audience");
    }
    const auto tail = Json::parse(request.messages.back().content);
    const auto& items = tail.at("items");
    const auto batch = std::find_if(items.begin(),items.end(),[](const auto& item){return item.at("key")=="memory.matches";});
    const bool searched = !followup && step>0 && row.contains("hits");
    require((batch!=items.end())==searched,"unexpected search projection");
    const auto expected = searched ? row.at("hits").get<std::vector<std::string>>() : std::vector<std::string>{};
    if (searched) {
        const auto& selection = batch->at("value");
        const auto& hits = selection.at("items");
        require(hits.size()==expected.size() && hits.size()<=3 && hits.dump().size()<=2048 &&
            selection.at("limited")==row.at("limited"),"selection size or limit differs");
        for (std::size_t i=0;i<hits.size();++i) {
            const auto& source = sources.at(expected[i]);
            require(hits[i].at("reference")==profile+"/"+std::to_string(source.id) && hits[i].at("source")==source.source &&
                hits[i].at("utcMs")==source.utc_ms && hits[i].at("quote")==source.envelope.at("text"),"different evidence or order");
        }
    }
    const auto wire = model::encode_request_fields(request,false);
    for (const auto& [key,source] : sources) {
        const auto& quote = source.envelope.at("text").get_ref<const std::string&>();
        const bool selected = std::ranges::find(expected,key)!=expected.end();
        require((wire.find(quote)!=std::string::npos)==selected,"wrong full-request visibility for "+key);
    }
}

// 从同一档案重开主聊天，播放固定的模型提议，并独立核对实际工具回执与送模材料。
// 步骤：绑定来源与受众 → 按正常入口交付 → 验证每步回执、下一轮及 Trace 摘要。
std::size_t run(const Json& row, const std::filesystem::path& data, Sources& sources) {
    const auto id = row.at("id").get<std::string>();
    const auto text = row.at("text").get<std::string>()+std::string(row.value("padding",0u),'A');
    const auto steps = row.value("steps",Json::array());
    const auto states = row.value("states",Json::array());
    const auto db = std::make_shared<app::MessageStore>(data);
    app::ApplicationRunConfig config;
    config.trace_directory = data/"traces"/id;
    bool followup = false;
    std::string previous_snapshot;
    std::vector<model::Request> requests;
    app::Application host({},data/"empty",data,model::Backend{[&](const auto&,const model::Request& request)->model::Response {
        requests.push_back(request);
        const auto step = Json::parse(request.messages.back().content).at("step").get<unsigned>()-1;
        if (followup && !step)
            return {{model::Role::assistant,"",{{"probe","status_read",Json{{"snapshot",previous_snapshot}}.dump()}}},model::FinishReason::tool_calls};
        if (!followup && step<steps.size()) {
            const auto action = steps.at(step).begin();
            return {{model::Role::assistant,"",{{"call"+std::to_string(step),action.key(),action.value().dump()}}},model::FinishReason::tool_calls};
        }
        return {{model::Role::assistant,"收到。"}};
    }},config);
    host.bind_messages(db,id,nullptr);
    auto& channel = *host.main_messages();
    if (row.contains("revoke"))
        channel.change_memory(db->conversation()+"/"+std::to_string(sources.at(row.at("revoke").get<std::string>()).id),std::nullopt);
    const auto audience = row.value("audience",std::string{"owner"});
    if (audience!="owner") host.post_room_event(app::RoomEvent::addressed,"书房",audience=="room" ? text : "待收的来信");
    if (audience=="room") require(host.review_room()==app::MailboxIntakeResult::submitted,"room not admitted");
    else channel.terminal(id,text);
    drain(host);
    require(requests.size()==steps.size()+1 && states.size()==steps.size(),"wrong number of decisions");
    for (unsigned i=0;i<requests.size();++i) {
        check_request(requests[i],sources,db->conversation(),row,i,false);
        if (!i) continue;
        const auto& messages = requests[i].messages;
        const auto& receipt = messages.at(messages.size()-2);
        require(receipt.role==model::Role::tool && receipt.tool_call_id=="call"+std::to_string(i-1),"unpaired tool receipt");
        const auto result = Json::parse(receipt.content.substr(receipt.content.find('{')));
        require(result.at("state")==states.at(i-1),"unexpected receipt: "+result.dump());
        if (result.at("state")=="saved") require(result.at("reference")==db->conversation()+"/"+std::to_string(db->find("terminal",id)->id),"wrong stored original");
    }
    if (row.contains("followup")) {
        previous_snapshot = Json::parse(requests.back().messages.back().content).at("snapshot");
        followup = true;
        channel.terminal(id+"-next",row.at("followup").get<std::string>());
        drain(host);
        require(requests.size()==steps.size()+3,"missing snapshot probe");
        check_request(requests.at(requests.size()-2),sources,db->conversation(),row,0,true);
        check_request(requests.back(),sources,db->conversation(),row,1,true);
        const auto& content = requests.back().messages.at(requests.back().messages.size()-2).content;
        const auto read = Json::parse(content.substr(content.find('{')));
        require(read.at("state")=="ready" && read.at("status").at("omittedDecisionOnly")==1,"historic snapshot bypassed search budget");
    }
    std::ifstream trace(host.trace_file(),std::ios::binary);
    std::size_t verified = 0;
    for (std::string line;std::getline(trace,line);) {
        const auto event = Json::parse(line);
        if (event.at("kind")!="model.prepared") continue;
        const auto wire = model::encode_request_fields(requests.at(verified++),event.at("thinking").get<bool>());
        require(platform::sha256(wire)==event.at("wireSha256").get<std::string>(),"prepared digest differs from backend request");
    }
    require(verified==requests.size(),"missing prepared request");
    if (!steps.empty() && steps.front().contains("memory_note")) sources.emplace(id,*db->find("terminal",id));
    return verified;
}
}

// 外置语料保留输入、模型提议与独立的证据答案，整套测试无需网络或模型服务。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc!=3) return 2;
        const auto data = std::filesystem::path(argv[2])/std::to_string(GetCurrentProcessId())/std::to_string(GetTickCount64());
        std::ifstream corpus(std::filesystem::path(argv[1]),std::ios::binary);
        Sources sources;
        unsigned cases = 0;
        std::size_t requests = 0;
        for (std::string line;std::getline(corpus,line);) {
            const auto row = Json::parse(line);
            try { requests += run(row,data,sources); }
            catch (const std::exception& e) { throw std::runtime_error(row.at("id").get<std::string>()+": "+e.what()); }
            ++cases;
        }
        require(cases>0,"empty dialogue corpus");
        std::cout << cases << " dialogue cases, " << requests << " verified backend requests\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
