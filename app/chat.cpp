#include "chat.hpp"
#include <iostream>
#include <stdexcept>

namespace dfy::app {
// 驱动终端问答，处理本地命令并允许模型调用失败后继续输入。
// 步骤：显示用法并读取输入 → 分派退出、清空与空行 → 执行一轮问答或报告错误。
int chat(agent::Session& session, std::istream& input, std::ostream& output, std::ostream& errors) {
    output << "DaFeiYu\nEnter a message, /clear to reset, or /exit to quit.\n";
    std::string line;
    while (output << "> " << std::flush && errors && std::getline(input, line)) {
        if (line.ends_with('\r')) {
            line.pop_back();
        }
        if (line == "/exit") {
            break;
        }
        if (line == "/clear") {
            session.clear();
            output << "Context cleared.\n";
            continue;
        }
        if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
            continue;
        }
        if (line.starts_with('/')) {
            errors << "Unknown command. Use /clear or /exit.\n";
            continue;
        }

        try {
            output << session.ask(line) << '\n';
        } catch (const std::exception& error) {
            errors << error.what() << '\n';
        }
        if (!errors) {
            return 1;
        }
    }
    return input.bad() || (input.fail() && !input.eof()) || !output || !errors ? 1 : 0;
}
}
