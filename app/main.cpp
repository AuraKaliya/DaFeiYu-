#include <iostream>
#include <string>

// 运行最小终端入口，接收退出命令或输入结束信号。
// 步骤：显示启动信息 → 读取并处理终端命令 → 返回进程退出状态。
int main() {
    std::cout << "DaFeiYu\nEnter /exit to quit.\n";

    std::string command;
    while (std::getline(std::cin, command)) {
        if (command == "/exit") {
            break;
        }
        if (!command.empty()) {
            std::cout << "Unknown command. Enter /exit to quit.\n";
        }
    }

    return std::cin.bad() ? 1 : 0;
}
