#include "chat.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using dfy::model::Message;
using dfy::model::Role;
std::vector<Message> received;
bool fail = false;
int calls = 0;

// 在会话或终端行为不符合预期时报告测试失败。
void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}
}

namespace dfy::model {
// 替代真实模型接口，捕获会话上下文并模拟成功回答或失败。
// 步骤：核对配置并记录请求 → 根据测试场景抛错或返回文本。
std::string complete(const Config& config, const std::vector<Message>& messages) {
    check(config.model == "test-model", "model configuration lost");
    ++calls;
    received = messages;

    if (fail || messages.back().content == "失败") {
        throw std::runtime_error("model: test failure");
    }
    return "回答：" + messages.back().content;
}
}

// 验证上下文提交、失败回滚、会话隔离和终端命令，不访问网络。
// 步骤：连续问答并模拟失败 → 检查清空和独立会话 → 驱动终端输入与流故障。
int main() {
    try {
        const dfy::model::Config config{"deepseek", "https://example.invalid", "test-model", "UNUSED_KEY", 1000};
        dfy::agent::Session session(config);
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

        dfy::agent::Session other(config);
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
        std::cout << "Agent session checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
