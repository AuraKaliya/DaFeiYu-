#include <dafeiyu/model_config.hpp>
#include <dafeiyu/model_client.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
// 读取有大小限制的提示词文件，兼容 UTF-8 BOM 并保留正文换行。
// 步骤：打开文件 → 有界读取并检查错误 → 去除编码签名。
std::string read_prompt(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("prompt: cannot open file");
    }

    std::string text(1024 * 1024 + 4, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));
    if (file.bad()) {
        throw std::runtime_error("prompt: cannot read file");
    }

    if (text.starts_with("\xEF\xBB\xBF")) {
        text.erase(0, 3);
    }
    return text;
}

// 分派配置检查、单次模型调用与最小终端入口，并统一报告错误。
// 步骤：检查启动参数并执行相应操作 → 读取终端命令 → 返回退出状态。
int run(const std::vector<std::filesystem::path>& args) {
    try {
        if (args.size() == 3 && args[1] == "--check-config") {
            const auto config = dfy::model::load_config(args[2]);
            std::cout << "Configuration valid: " << config.provider << " / " << config.model
                      << "\nCredentials and service availability have not been checked.\n";
            return 0;
        }
        if (args.size() == 4 && args[1] == "--complete") {
            const auto config = dfy::model::load_config(args[2]);
            const auto prompt = read_prompt(args[3]);
            std::cout << dfy::model::complete(config, prompt) << '\n';
            return 0;
        }
        if (args.size() != 1) {
            std::cerr << "Usage: dafeiyu [--check-config FILE | --complete CONFIG PROMPT_FILE]\n";
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
