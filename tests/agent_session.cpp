#include "chat.hpp"
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace {
using dfy::model::Message;
using dfy::model::Role;
std::vector<Message> received;
bool fail = false;
int calls = 0;
std::function<void()> during_model;

// 在会话或终端行为不符合预期时报告测试失败。
void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}
}

namespace dfy::model {
// 替代真实模型接口，捕获会话上下文并模拟成功回答或失败。
// 步骤：核对配置并记录请求 → 执行调用中的重入检查 → 根据测试场景抛错或返回文本。
std::string complete(const Config& config, const std::vector<Message>& messages) {
    check(config.model == "test-model", "model configuration lost");
    ++calls;
    received = messages;

    if (during_model) {
        during_model();
    }
    if (messages.back().content == "非标准异常") {
        throw 42;
    }
    if (fail || messages.back().content == "失败") {
        throw std::runtime_error("model: test failure");
    }
    return "回答：" + messages.back().content;
}
}

namespace {
// 验证忙碌时问答和清空都被明确拒绝，不允许悄悄修改状态。
// 步骤：分别尝试问答和清空 → 核对忙碌错误。
void rejects_busy(dfy::agent::Session& session) {
    for (bool clear : {false, true}) {
        bool rejected = false;
        try {
            if (clear) {
                session.clear();
            } else {
                (void)session.ask("重入提问");
            }
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()) == "session: busy";
        }
        check(rejected, "busy session operation accepted");
    }
}

// 验证通知中的业务提交状态、回合关联、重入拒绝和观察者失败隔离。
// 步骤：注册失败与重入观察者 → 执行成功和失败回合 → 清空并继续 → 核对完整事件序列。
void session_events(const dfy::model::Config& config) {
    using namespace dfy::events;
    static_assert(!std::is_copy_constructible_v<dfy::agent::Session> &&
                  !std::is_move_constructible_v<dfy::agent::Session>);
    Bus bus;
    dfy::agent::Session session(config, bus), other(config, bus);
    check(session.id() != 0 && other.id() != 0 && session.id() != other.id(), "session identity reused");
    const auto before = calls;
    std::vector<Event> trace;
    std::vector<std::size_t> sizes;
    std::vector<int> model_calls;
    bus.subscribe(10, [](const Event&) { throw std::runtime_error("observer failure"); });
    bus.subscribe(20, [&](const Event&) { session.clear(); });
    bus.subscribe(30, [&](const Event&) { rejects_busy(session); rejects_busy(other); });
    bus.subscribe(40, [&](const Event& event) {
        trace.push_back(event);
        sizes.push_back(event.session_id == session.id() ? session.message_count() : other.message_count());
        model_calls.push_back(calls - before);
    });

    int model_reentries = 0;
    during_model = [&] { rejects_busy(session); ++model_reentries; };
    check(session.ask("第一问") == "回答：第一问", "observer changed successful answer");
    during_model = {};
    const auto reports = session.last_notifications();
    check(model_reentries == 1 && reports.started.failed == 2 && reports.finished.failed == 2 &&
              reports.started.first_failed_owner == 10 && reports.finished.first_failed_owner == 10,
        "notification failures lost or reentry overwrote reports");
    try {
        (void)session.ask("失败");
        throw std::logic_error("expected model failure");
    } catch (const std::runtime_error& error) {
        check(std::string(error.what()) == "model: test failure", "observer replaced model error");
    }
    (void)session.ask("第二问");
    check(received.size() == 3 && received[0].content == "第一问" && received[2].content == "第二问",
        "failed turn or reentry polluted history");
    bool nonstandard = false;
    try {
        (void)session.ask("非标准异常");
    } catch (int value) {
        nonstandard = value == 42;
    }
    check(nonstandard, "nonstandard model error changed");

    session.clear();
    session.clear();
    check(calls == before + 4 && session.message_count() == 0 &&
              session.last_notifications().started.failed == 0 && session.last_notifications().finished.failed == 2,
        "clear called model or lost diagnostics");
    (void)session.ask("清空后提问");
    check(received.size() == 1, "clear retained history");
    (void)other.ask("独立提问");
    check(received.size() == 1, "events mixed session histories");

    const std::vector<Kind> kinds{Kind::turn_started, Kind::turn_completed, Kind::turn_started, Kind::turn_failed,
        Kind::turn_started, Kind::turn_completed, Kind::turn_started, Kind::turn_failed,
        Kind::session_cleared, Kind::session_cleared, Kind::turn_started, Kind::turn_completed,
        Kind::turn_started, Kind::turn_completed};
    const std::vector<std::uint64_t> turns{1, 1, 2, 2, 3, 3, 4, 4, 0, 0, 5, 5, 1, 1};
    check(trace.size() == kinds.size(), "turn emitted missing or duplicate terminal events");
    for (std::size_t i = 0; i < trace.size(); ++i) {
        check(trace[i].kind == kinds[i] && trace[i].turn_id == turns[i] &&
                  trace[i].session_id == (i < 12 ? session.id() : other.id()), "event association incorrect");
    }
    check(sizes == std::vector<std::size_t>{0, 2, 2, 2, 2, 4, 4, 4, 0, 0, 0, 2, 0, 2},
        "notification preceded commit or failure changed history");
    check(model_calls == std::vector<int>{0, 1, 1, 2, 2, 3, 3, 4, 4, 4, 4, 5, 5, 6},
        "start notification occurred after model or reentry called model");
}
}

// 验证上下文、终端命令和会话事件，不访问网络。
// 步骤：连续问答并模拟失败 → 检查清空和独立会话 → 驱动终端输入与流故障 → 检查事件与重入边界。
int main() {
    try {
        const dfy::model::Config config{"deepseek", "https://example.invalid", "test-model", "UNUSED_KEY", 1000};
        dfy::events::Bus events;
        dfy::agent::Session session(config, events);
        check(session.ask("第一问") == "回答：第一问" && received.size() == 1, "first turn failed");
        fail = true;
        try {
            (void)session.ask("失败提问");
            throw std::logic_error("expected model failure");
        } catch (const std::runtime_error&) {
        }
        fail = false;
        (void)session.ask("第二问");
        check(received.size() == 3 && received[0].role == Role::user && received[0].content == "第一问" &&
                  received[1].role == Role::assistant && received[1].content == "回答：第一问" &&
                  received[2].content == "第二问",
            "failed turn polluted history");
        (void)session.ask("第三问");
        check(received.size() == 5 && received[3].content == "回答：第二问", "successful turn not retained");

        dfy::agent::Session other(config, events);
        (void)other.ask("独立提问");
        check(received.size() == 1, "sessions share history");
        session.clear();
        (void)session.ask("新提问");
        check(received.size() == 1 && received[0].content == "新提问", "clear retained old history");

        session.clear();
        const auto before_chat = calls;
        std::istringstream input(" \t\n/unknown\n你好\r\n失败\n再问\n/clear\n重新开始\n/exit\n不要调用\n");
        std::ostringstream output, errors;
        check(dfy::app::chat(session, input, output, errors) == 0, "normal chat exit failed");
        check(calls == before_chat + 4 && received.size() == 1 && received[0].content == "重新开始",
            "terminal commands reached model or clear failed");
        check(output.str().find("回答：你好") != std::string::npos &&
                  output.str().find("回答：再问") != std::string::npos &&
                  output.str().find("Context cleared.") != std::string::npos,
            "terminal output missing");
        check(errors.str().find("Unknown command") != std::string::npos &&
                  errors.str().find("model: test failure") != std::string::npos,
            "terminal errors missing");

        const auto before_eof = calls;
        std::istringstream eof;
        check(dfy::app::chat(session, eof, output, errors) == 0 && calls == before_eof, "EOF called model");
        std::istringstream broken_input;
        broken_input.setstate(std::ios::badbit);
        check(dfy::app::chat(session, broken_input, output, errors) == 1, "input failure ignored");
        std::istringstream unused("不要调用\n");
        output.setstate(std::ios::badbit);
        check(dfy::app::chat(session, unused, output, errors) == 1 && calls == before_eof,
            "output failure allowed model call");
        session_events(config);
        std::cout << "Agent session checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
