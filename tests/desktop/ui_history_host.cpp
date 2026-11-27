#include "application.hpp"
#include "management/resident.hpp"
#include "message_store.hpp"
#include "ui_fixture_messages.hpp"
#include <dafeiyu/digest.hpp>
#include <dafeiyu/model_protocol.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace dfy;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
constexpr char held[] = "两个窗口关闭后再回答";
constexpr char task_reply[] = "已核对所选任务的原回执。";
using Json = nlohmann::json;

// 从原任务进程留下的证据校验重启后实收的第一个模型请求。
// 步骤：挂起一封房间来信 → 核对私聊数据和 T03 原件 → 保存实收摘要并返回固定答复。
std::unique_ptr<app::Application> open_task_resume(const fs::path& data) {
    std::ifstream proof(data.parent_path()/"task.json",std::ios::binary);
    app::ApplicationRunConfig options;
    options.trace_directory = data/"task-resume-trace";
    options.wake_for_room_address = false;
    options.tool_clock = [] { return std::chrono::steady_clock::time_point{}; };
    auto host = std::make_unique<app::Application>(model::Config{},data/"plugins",data,model::Backend{
        // 固定替身核对实收首包，完整的证据才得到答复。
        // 步骤：核对受众 → 比对原任务 → 编码并记录实收请求。
        [data,expected=Json::parse(proof)](const auto& config, const model::Request& request) -> model::Response {
            const auto file = data/"first-request.json";
            if (fs::exists(file) || request.messages.empty() || request.messages.back().source!=model::MessageSource::host_status ||
                std::ranges::any_of(request.messages,[](const auto& message){return message.source==model::MessageSource::mailbox;}))
                throw std::runtime_error("expected one owner-private model request");
            const auto tail = Json::parse(request.messages.back().content);
            const auto& samples = tail.at("items");
            const auto sample = std::ranges::find_if(samples,[](const auto& item){return item.at("key")=="memory.tasks";});
            if (sample==samples.end() || sample->at("value").at("items").size()!=1)
                throw std::runtime_error("first request did not contain exactly one task");
            const auto& recalled = sample->at("value").at("items").front();
            for (const auto key : {"reference","goal","expected","effect","verification","actionRef","receiptRef","readbackRef"})
                if (recalled.at(key)!=expected.at(key)) throw std::runtime_error(std::string("selected T03 differs at ")+key);
            if (recalled.at("review").get_ref<const std::string&>().find("动作后读回「"+expected.at("after").get<std::string>()+"」")==std::string::npos)
                throw std::runtime_error("recalled review differs from the original readback");
            const auto wire = model::encode_request_fields(request,config.thinking_enabled());
            std::ofstream evidence(file,std::ios::binary);
            evidence << Json{{"firstEvidenceRequest",1},{"wireBytes",wire.size()},{"wireSha256",platform::sha256(wire)},
                {"reference",recalled.at("reference")},{"readbackRef",recalled.at("readbackRef")},
                {"verification",recalled.at("verification")},{"expected",expected.at("expected")},{"readback",expected.at("after")}}.dump();
            evidence.close();
            if (!evidence) throw std::runtime_error("could not save first request evidence");
            return {{model::Role::assistant,task_reply}};
        }},options);
    if (host->post_room_event(app::RoomEvent::addressed,"书房","等待接收的房间来信").state!=events::DeliveryState::accepted)
        throw std::runtime_error("room message was not queued");
    return host;
}

// 独立读回正式答复及 Trace，核对首次实收请求和同号投递。
// 步骤：查询原桌面提交 → 对照首包摘要与实际准备事件 → 核实没有收到房间来信或启动旧动作。
void verify_task_resume(const fs::path& data) {
    std::ifstream evidence(data/"first-request.json",std::ios::binary);
    const auto report = Json::parse(evidence);
    const app::MessageStore db(data);
    const auto page = db.page(true,0,db.watermark(),2);
    const auto& input = page.records.at(1);
    const auto& answer = page.records.front();
    if (input.source!="desktop" || input.envelope.at("resumeTask")!=report.at("reference") || input.receipt.at("state")!="accepted" ||
        answer.envelope.at("replyTo")!=std::to_string(input.id) || answer.envelope.at("text")!=task_reply || answer.receipt.at("state")!="delivered")
        throw std::runtime_error("owner reply does not belong to the selected desktop submission");
    std::ifstream trace(data/"task-resume-trace"/"trace"/"events.jsonl",std::ios::binary);
    unsigned prepared = 0, room_accepted = 0;
    for (std::string line; std::getline(trace,line);) {
        const auto event = Json::parse(line);
        const auto& kind = event.at("kind");
        if (kind=="model.prepared") {
            ++prepared;
            if (event.at("wireSha256")!=report.at("wireSha256") ||
                event.at("run").dump()!=input.receipt.at("turn").get_ref<const std::string&>())
                throw std::runtime_error("prepared trace differs from the backend request or original turn");
        }
        if (kind=="tool.candidate" || (kind=="mail.received" && event.at("category")=="room.addressed"))
            throw std::runtime_error("private resume replayed a tool or consumed room mail");
        if (kind=="mail.accepted" && event.at("category")=="room.addressed") ++room_accepted;
    }
    if (prepared!=1 || room_accepted!=1) throw std::runtime_error("expected one first request with room mail still pending");
    std::cout << report.dump() << '\n';
}

// 最后一轮等两个窗口进程退出后才生成答复，所有问答都经过实际应用。
std::unique_ptr<app::Application> open_fixture(const fs::path& data) {
    return std::make_unique<app::Application>(model::Config{},data/"plugins",data,model::Backend{
        [data](const auto&, const auto& request) -> model::Response {
            const auto& last = tests::fixture_business_messages(request.messages).back();
            const auto& text = last.content;
            if (last.role==model::Role::user && text=="两次独立的同一句")
                return {{model::Role::assistant,"",{{"speak","main_reply",R"({"text":"固定回复：两次独立的同一句"})"}}},model::FinishReason::tool_calls};
            if (last.role==model::Role::tool) return {{model::Role::assistant,"固定回复：两次独立的同一句"}};
            if (text==held) {
                std::ofstream(data/"reply-held").put('1');
                const auto deadline = std::chrono::steady_clock::now()+35s;
                while (!fs::is_regular_file(data/"release-reply")) {
                    if (std::chrono::steady_clock::now()>=deadline) throw std::runtime_error("closed-window gate timed out");
                    std::this_thread::sleep_for(20ms);
                }
            }
            return {{model::Role::assistant,"固定回复："+text}};
        }});
}

// 在没有窗口的阶段直接读已提交记录，核对唯一投递、原消息关联和实际回复数。
void verify_closed(const fs::path& data) {
    const app::MessageStore db(data);
    const auto deadline = std::chrono::steady_clock::now()+8s;
    do {
        const auto page = db.page(true,0,db.watermark(),64);
        if (page.records.size()==38) {
            const auto& answer = page.records[0];
            const auto& original = page.records[1];
            if (answer.envelope.at("text")!=std::string("固定回复：")+held || answer.source!="main" ||
                original.envelope.at("text")!=held || original.source!="desktop" ||
                answer.envelope.at("replyTo")!=std::to_string(original.id) || answer.receipt.at("state")!="delivered")
                throw std::runtime_error("closed-window answer or its origin differs");
            unsigned spoken = 0;
            for (const auto& row : page.records) {
                if (row.source=="main" && row.envelope.at("text")=="固定回复：两次独立的同一句") {
                    if (row.submission.find("/tool-")==std::string::npos) throw std::runtime_error("expected a committed explicit delivery");
                    ++spoken;
                }
            }
            if (spoken!=2) throw std::runtime_error("independent explicit replies differ");
            return;
        }
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now()<deadline);
    throw std::runtime_error("expected 19 persisted pairs before reopening");
}
}

// 由验收脚本选择隔离本体、只读核对或构造旧实例的未决回执样本。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc!=4) return 2;
        const std::wstring_view mode(argv[1]);
        const fs::path config(argv[2]), data(argv[3]);
        if (mode==L"serve") return management::serve_resident(data,management::startup_identity(config,data/"plugins"),
            [data] { return open_fixture(data); });
        if (mode==L"serve-task-resume") return management::serve_resident(data,management::startup_identity(config,data/"plugins"),
            [data] { return open_task_resume(data); });
        if (mode==L"verify-task-resume") verify_task_resume(data);
        else if (mode==L"verify-closed") verify_closed(data);
        else if (mode==L"seed-interrupted") {
            app::MessageStore db(data);
            db.put("desktop","interrupted-original",{{"sender","user"},{"route","send"},{"text","重启前待核对的原话"}},
                {{"state","dispatching"},{"instance","previous-fixture"}});
        } else return 2;
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
