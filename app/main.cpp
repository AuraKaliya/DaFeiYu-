#include <dafeiyu/model_config.hpp>
#include <iostream>
#include <string>
#include <vector>

namespace {
// 根据启动参数校验模型配置，或进入最小终端入口。
// 步骤：分派配置检查与参数错误 → 读取终端命令 → 返回退出状态或报告配置错误。
int run(const std::vector<std::filesystem::path>& args) {
    try {
        if (args.size() == 3 && args[1] == "--check-config") {
            const auto config = dfy::model::load_config(args[2]);
            std::cout << "Configuration valid: " << config.provider << " / " << config.model
                      << "\nCredentials and service availability have not been checked.\n";
            return 0;
        }
        if (args.size() != 1) {
            std::cerr << "Usage: dafeiyu [--check-config FILE]\n";
            return 1;
        }

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
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
}

#ifdef _WIN32
// 使用宽字符路径接收 Windows 参数，保留中文配置文件名。
int wmain(int argc, wchar_t** argv) { return run(std::vector<std::filesystem::path>(argv, argv + argc)); }
#else
// 将命令行参数转换为路径并交给统一入口。
int main(int argc, char** argv) { return run(std::vector<std::filesystem::path>(argv, argv + argc)); }
#endif
