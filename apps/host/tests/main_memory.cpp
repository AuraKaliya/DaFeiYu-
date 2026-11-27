#include "application.hpp"
#include "application_commands.hpp"
#include "main_messages.hpp"
#include <dafeiyu/digest.hpp>
#include <dafeiyu/model_protocol.hpp>
#include <atomic>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace {
using namespace dfy;
using Json = nlohmann::json;
const std::string secret = "周报的内部标记是白桦-741。";
const std::string original_text = "记住 周报："+secret;
const std::string older_quote = "周四交付的独立原话。";

// 让断言指出具体的身份、受众或投递阶段。
void require(bool value, const char* error) {
    if (!value) throw std::runtime_error(error);
}

// 通过管理终端的正式命令分派更改权限或查询原件身份。
std::string command(app::Application& app, const std::string& text) {
    std::ostringstream out, err;
    require(app::application_command(app,text,out,err) && err.str().empty(),"memory command not handled");
    auto result = out.str();
    if (result.ends_with('\n')) result.pop_back();
    return result;
}

// 在有限时间内推进真实宿主，等待谓词对应的业务状态。
template<class Predicate> void until(app::Application& app, Predicate done) {
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(6);
    while (!done()) {
        app.advance_plugins();
        app.conversation().advance();
        require(std::chrono::steady_clock::now()<end,"main memory fixture timed out");
        std::this_thread::yield();
    }
}

// 验证每个实际后端请求都匹配 Trace 的准备记录，并检查完整请求中的受限正文。
void verify_trace(const std::filesystem::path& path, const std::vector<model::Request>& requests,
    const std::vector<bool>& expected) {
    require(requests.size()==expected.size(),"unexpected number of backend requests");
    std::ifstream file(path,std::ios::binary);
    std::size_t n = 0;
    for (std::string line; std::getline(file,line);) {
        const auto row = Json::parse(line);
        if (row.at("kind")!="model.prepared") continue;
        require(n<requests.size(),"extra prepared request");
        const auto wire = model::encode_request_fields(requests[n],row.at("thinking").get<bool>());
        require(platform::sha256(wire)==row.at("wireSha256").get<std::string>(),"backend and prepared request differ");
        require((wire.find(secret)!=wire.npos)==expected[n] &&
            (row.at("input").dump().find(secret)!=std::string::npos)==expected[n],"wrong audience received original");
        const auto tail = Json::parse(requests[n].messages.back().content);
        for (const auto& item : tail.at("items")) if (item.at("key")=="memory.original")
            std::cout << "turn=" << row.at("run") << " memory_item_bytes=" << item.dump().size()
                << " encoded_request_bytes=" << wire.size() << '\n';
        ++n;
    }
    require(n==expected.size(),"missing prepared requests");
    std::cout << "verified " << n << " prepared requests against backend input\n";
}

// 在独立进程中依次收录、查回和复查撤权，用当前调用者实际能看到的输入裁定。
// 步骤：写入带来源与时间的原话 → 在不同入口取证 → 撤回在途读取并在再启动后核对。
void verify(const std::filesystem::path& profile, std::wstring_view phase) {
    auto db = std::make_shared<app::MessageStore>(profile);
    app::ApplicationRunConfig config;
    config.trace_directory = profile/"trace"/phase;
    std::vector<model::Request> requests;
    std::vector<bool> expected;
    std::string snapshot;
    bool probe_old = false;
    std::atomic_bool hold = false, entered = false;
    app::Application app({},profile/"empty",profile,model::Backend{[&](const auto&,const model::Request& request) -> model::Response {
        requests.push_back(request);
        const auto status = Json::parse(request.messages.back().content);
        if (hold) {
            snapshot = status.at("snapshot");
            entered = true;
            const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(4);
            while (hold && std::chrono::steady_clock::now()<end) std::this_thread::yield();
            return {{model::Role::assistant,"",{{"late","main_reply",R"({"text":"late reply"})"}}},model::FinishReason::tool_calls};
        }
        if (probe_old) {
            probe_old = false;
            return {{model::Role::assistant,"",{{"old","status_read",Json{{"snapshot",snapshot}}.dump()}}},model::FinishReason::tool_calls};
        }
        for (const auto& item : status.at("items")) if (item.at("key")=="memory.original") {
            snapshot = status.at("snapshot");
            const auto& value = item.at("value");
            require(value.at("reference")==db->conversation()+"/"+value.at("message").get<std::string>(),
                "original reference changed");
            if (value.at("submission")=="existing-source") {
                require(value.at("quote")==older_quote && value.at("source")=="terminal","wrong retained historic quote");
                return {{model::Role::assistant,older_quote}};
            }
            require(value.at("source")=="desktop" && value.at("submission")=="original" &&
                value.at("conversation")==db->conversation() && value.at("message")=="1" &&
                item.at("sampledUtcMs")==12345 && value.at("quote")==original_text,"wrong memory or source time");
            return {{model::Role::assistant,secret}};
        }
        return {{model::Role::assistant,"收到。"}};
    }},config);
    app.bind_messages(db,std::filesystem::path(phase).string(),nullptr);
    auto& channel = *app.main_messages();
    const auto drain = [&] { until(app,[&]{return !app.conversation().busy();}); };
    const auto send = [&](const std::string& id,const std::string& text) { channel.terminal(id,text); drain(); };
    const auto identity_file = profile/"identity.txt";
    std::string identity;
    if (phase==L"save") {
        const auto envelope = app::user_envelope("send",original_text,{}, {},12345);
        const auto original = db->put("desktop","original",envelope,{{"state","pending"}});
        require(channel.deliver(original.message.id,envelope,original.message.utc_ms).at("state")=="accepted","original delivery failed");
        drain();
        identity = command(app,"/memory last");
        require(identity==db->conversation()+"/1" && !snapshot.empty(),"wrong stable identity or missing save receipt");
        std::ofstream(identity_file) << identity;
        // 单靠桌面写入就应在下一进程可查；此处只验证原号重投。
        require(!db->put("desktop","original",envelope,{{"state","pending"}}).created,"original duplicated");
        expected = {true};
    } else {
        std::ifstream(identity_file) >> identity;
        require(identity==db->conversation()+"/1","restart changed profile or original");
        if (phase==L"reopen") {
            send("by-id","回想 "+identity);
            require(!snapshot.empty(),"missing private recall");
            // 在固定答复中原样复述秘密，后续断言同时覆盖历史、投递尾注和状态工具。
            app.post_room_event(app::RoomEvent::addressed,"书房","回想 "+identity);
            require(app.review_room()==app::MailboxIntakeResult::submitted,"room not admitted");
            drain();
            send("by-topic","回想 周报");
            app.post_room_event(app::RoomEvent::addressed,"书房","回想 周报");
            send("mixed","回想 周报");
            send("foreign","回想 00000000000000000000000000000000/1");
            require(requests.back().messages.back().content.find("memory.original")==std::string::npos,"foreign original admitted");
            hold = true;
            channel.terminal("in-flight","回想 周报");
            until(app,[&]{return entered.load();});
            command(app,"/memory revoke "+identity);
            hold = false;
            drain();
            require(app.conversation().status()->result==runtime::OperationState::cancelled,"private continuation survived revocation");
            for (const auto& row : db->page(true,0,db->watermark(),64).records)
                require(row.envelope.at("text")!="late reply","cancelled run delivered a late tool call");
            probe_old = true;
            send("after-revoke","回想 "+identity);
            require(model::encode_request_fields(requests.back(),false).find("scope_mismatch")!=std::string::npos,
                "old snapshot reached new binding");
            expected = {true,false,true,false,false,true,false,false};
        } else {
            require(phase==L"revoked","unknown fixture phase");
            send("idempotent-revoke","撤回记忆 "+identity);
            bool refused = false;
            try { command(app,"/memory keep "+identity+" 周报"); } catch (const std::invalid_argument&) { refused = true; }
            require(refused,"stale retry revived permission");
            send("still-revoked","回想 周报");
            send("existing-source",older_quote);
            const auto existing = command(app,"/memory last");
            command(app,"/memory keep "+existing+" 已有原话");
            command(app,"/memory keep "+existing+" 已有原话");
            send("existing-read","回想 已有原话");
            require(requests.back().messages.back().content.find(older_quote)!=std::string::npos,"manual capture missing");
            send("existing-revoke","撤回记忆 "+existing);
            send("existing-denied","回想 "+existing);
            require(model::encode_request_fields(requests.back(),false).find(older_quote)==std::string::npos,
                "chat revocation leaked previous quote");
            expected = {false,false,false,false,false,false};
        }
    }
    verify_trace(app.trace_file(),requests,expected);
}
}
// CTest 为各次启动提供同一隔离档案，所有模型调用均由固定后端接收。
int wmain(int argc,wchar_t** argv) {
    try {
        if (argc!=3) return 2;
        verify(argv[1],argv[2]);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
