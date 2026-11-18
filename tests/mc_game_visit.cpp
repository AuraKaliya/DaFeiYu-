#include "application.hpp"
#include "application_commands.hpp"
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

namespace {
using namespace dfy;
// 报告正式会话与真实游戏证据不符。
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
// 在限定时间内推进正式应用而不直接写游戏、消息或计划。
void until(app::Application& app, const auto& ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
        app.advance_plugins();
        app.conversation().advance();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    check(ready(), "game delegation timeout");
}

// 按正式回合输入选择固定响应，保留跳过工具及非法改参数两条对照。
model::Response fixture_model(const model::Request& request, bool skip, bool tamper) {
    using model::Role;
    if (request.messages.back().role == Role::tool || skip) return {{Role::assistant, "我已发现石料"}};
    const auto& text = request.messages.back().content;
    check(request.tools.size() == 1 && text.starts_with("去山脚看看"), "unexpected player or tool scope");
    auto args = nlohmann::json::parse(text.substr(text.rfind('\n') + 1));
    if (tamper) args["order"] = 1;
    return {{Role::assistant, "", {{"visit", request.tools[0].name, args.dump()}}}, model::FinishReason::tool_calls};
}
}

// 用真实游戏原话建立子会话，仅接受原工具执行后的观察。
// 步骤：绑定独立应用和世界 → 等待新玩家消息 → 校验真实回执或拒绝 → 检查完整退出。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3 || argc == 4, "package, test root, optional variant required");
        const bool skip = argc == 4 && std::wstring_view(argv[3]) == L"skip";
        const bool tamper = argc == 4 && std::wstring_view(argv[3]) == L"tamper";
        const bool decline = argc == 4 && std::wstring_view(argv[3]) == L"decline";
        const bool lost_send = argc == 4 && std::wstring_view(argv[3]) == L"lost-send";
        const bool lost_receipt = argc == 4 && std::wstring_view(argv[3]) == L"lost-receipt";
        const bool game_answer = lost_send || lost_receipt || (argc == 4 && std::wstring_view(argv[3]) == L"approve-game");
        const bool center_answer = argc == 4 && std::wstring_view(argv[3]) == L"approve-center";
        const std::filesystem::path root = argv[2];
        app::Application application({}, root / "packages", root / "user", model::Backend{
            [=](const auto&, const auto& request) { return fixture_model(request, skip, tamper); }});
        application.plugins().install(argv[1]);
        check(application.plugins().load("mc.companion") == 0, "plugin load");
        std::ostringstream terminal;
        check(app::application_command(application, "/companion open mc.companion", terminal, std::cerr), "terminal open");
        const auto id = application.companion_status()->session;
        until(application, [&] { return application.companion_inbox(id)->ready; });
        check(application.companion_inbox(id)->cursor == 1, "wrong baseline");
        std::cout << "DFY_LISTENING" << std::endl;
        until(application, [&] { return application.companion_messages(id, 0).messages.size() >= 2; });
        const auto visit = application.companion_visit(id);
        check(visit && visit->order == 2 && visit->task == 1 && visit->text == "去山脚看看" && visit->run &&
            visit->address.session == id && visit->owner == application.companion_inbox(id)->owner, "player causality");
        const auto report = application.companion_messages(id, 1).messages.front();
        if (skip || tamper) {
            check(visit->phase == app::VisitPhase::failed && visit->evidence.empty() && report.kind == app::CompanionMessageKind::failed,
                "model text or modified target became evidence");
        } else {
            const auto evidence = nlohmann::json::parse(visit->evidence);
            check(visit->phase == app::VisitPhase::observed && evidence.at("moved") == true && evidence.at("observed") == true &&
                evidence.at("stone").at("x") == 15 && report.kind == app::CompanionMessageKind::report && report.reply_to == visit->task,
                "missing original observation");
            std::cout << evidence.dump() << '\n';
        }
        check(app::application_command(application, "/companion status", terminal, std::cerr), "terminal status");
        check(terminal.str().find(skip || tamper ? "unverified" : "observed") != std::string::npos, "terminal missing evidence state");
        if (decline) {
            until(application, [&] { const auto q = application.companion_question(id); return q && q->phase == app::QuestionPhase::waiting; });
            const auto original = *application.companion_question(id);
            check(original.owner == visit->owner && original.task == visit->task && original.after >= visit->order && original.revision == 2,
                "question not bound to original player and observation");
            app::application_command(application, "/companion status", terminal, std::cerr);
            check(terminal.str().find(original.text) != std::string::npos, "trusted surface did not see same question");
            std::cout << "DFY_QUESTION" << std::endl;
            until(application, [&] { return application.companion_question(id)->phase == app::QuestionPhase::declined; });
            const auto closed = *application.companion_question(id);
            check(closed.id == original.id && closed.revision == original.revision + 1 &&
                application.companion_visit(id)->phase == app::VisitPhase::observed, "decline altered original visit");
            std::cout << nlohmann::json{{"question", closed.id}, {"revision", closed.revision}, {"declined", true}, {"text", closed.text}}.dump() << '\n';
        }
        if (game_answer || center_answer) {
            until(application, [&] { const auto q = application.companion_question(id); return q && q->phase == app::QuestionPhase::waiting; });
            const auto q = *application.companion_question(id);
            const auto address = *application.companion_address();
            if (center_answer) {
                const auto command = "/companion approve " + std::to_string(id) + " " + std::to_string(q.id) + " " +
                    std::to_string(q.revision) + " 1 16";
                check(app::application_command(application, command, terminal, std::cerr), "management answer not admitted");
            }
            std::cout << "DFY_QUESTION" << std::endl;
            until(application, [&] { return application.companion_collection(id) && application.companion_inbox(id)->cursor > q.after; });
            const auto grant = *application.companion_collection(id);
            check(grant.quantity == 16 && grant.material == "minecraft:cobblestone" && grant.return_place == "riverside" &&
                grant.question == q.id && grant.owner == q.owner && grant.source_task == visit->task && grant.address == address &&
                grant.entry == (game_answer ? app::AnswerEntry::game : app::AnswerEntry::management), "grant lost scope");
            check(application.authorize_companion(address, q.id, q.revision, 1, 16).id == grant.id, "duplicate created new collection");
            bool conflict = false;
            try { application.authorize_companion(address, q.id, q.revision, 1, 12); }
            catch (const std::invalid_argument&) { conflict = true; }
            check(conflict && application.companion_question(id)->revision == q.revision + 1, "changed quantity accepted");
            auto stale = address;
            ++stale.binding;
            bool refused = false;
            try { application.authorize_companion(stale, q.id, q.revision, 2, 16); }
            catch (const std::invalid_argument&) { refused = true; }
            check(refused, "old game answer accepted");
            until(application, [&] { return application.companion_delivery(id)->phase == app::DeliveryPhase::accepted; });
            const auto delivered = *application.companion_delivery(id);
            check(delivered.grant == grant.id && delivered.submissions == (lost_send ? 2u : 1u) &&
                delivered.checks == (lost_send || lost_receipt ? 1u : 0u), "incorrect delivery recovery");
            std::cout << nlohmann::json{{"collection", grant.id}, {"question", grant.question}, {"quantity", grant.quantity},
                {"source", game_answer ? "game" : "management"}, {"return", grant.return_place}, {"accepted", true},
                {"submissions", delivered.submissions}, {"checks", delivered.checks}}.dump() << '\n';
        }
        check(app::application_command(application, "/companion stop " + std::to_string(id), terminal, std::cerr), "terminal stop");
        until(application, [&] { return application.companion_status()->closed; });
        application.conversation().close();
        check(application.close_plugins() == plugins::ShutdownResult::closed, "shutdown failed");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
