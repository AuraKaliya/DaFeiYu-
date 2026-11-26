#include "application.hpp"
#include "management/resident.hpp"
#include "message_store.hpp"
#include "ui_fixture_messages.hpp"
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace dfy;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
constexpr char held[] = "两个窗口关闭后再回答";

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
        if (mode==L"verify-closed") verify_closed(data);
        else if (mode==L"seed-interrupted") {
            app::MessageStore db(data);
            db.put("desktop","interrupted-original",{{"sender","user"},{"route","send"},{"text","重启前待核对的原话"}},
                {{"state","dispatching"},{"instance","previous-fixture"}});
        } else return 2;
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
