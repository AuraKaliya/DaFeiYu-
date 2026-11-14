#include "chat.hpp"
#include "plugin_commands.hpp"
#include "paths.hpp"
#ifdef _WIN32
#include "console.hpp"
#endif
#include <dafeiyu/model_config.hpp>
#include <dafeiyu/model_client.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>

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

// 分派配置检查、单次调用与 Agent 会话，并统一报告启动错误。
// 步骤：准备控制台编码 → 按参数启动检查或调用流程 → 返回退出状态。
int run(const std::vector<std::filesystem::path>& args) {
    try {
#ifdef _WIN32
        const dfy::app::Utf8Console console;
#endif

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
        if (args.size() == 1 || (args.size() >= 3 && args[1] == "--chat")) {
            const auto configuration = args.size() == 1 ? dfy::app::executable_directory()/"config"/"deepseek.example.json" : args[2];
            std::filesystem::path plugins, user_data;
            for (std::size_t index = 3; index < args.size(); index += 2) {
                if (index+1 >= args.size() || args[index+1].empty()) throw std::invalid_argument("app: option path required");
                if (args[index] == "--plugins" && plugins.empty()) plugins = args[index+1];
                else if (args[index] == "--data" && user_data.empty()) user_data = args[index+1];
                else throw std::invalid_argument("app: unknown or duplicate option");
            }
            if (plugins.empty()) plugins = dfy::app::executable_directory()/"plugins";
            if (user_data.empty()) user_data = dfy::app::default_user_data();
            auto store = std::make_shared<dfy::events::EventStore>(256,64);
            auto channel = std::make_shared<dfy::events::BroadcastChannel>(store,16,8);
            dfy::agent::AsyncSession session(dfy::model::load_config(configuration),channel);
            dfy::plugins::PluginManager manager(plugins,user_data,channel);
            dfy::platform::LineInput input;
            if (!manager.directory_error().empty()) std::cerr << "Plugin directory unavailable; chat remains available: " << manager.directory_error() << '\n';
            const auto result = dfy::app::async_chat(session,input,std::cout,std::cerr,[&] {
                if (manager.advance() == dfy::runtime::RunResult::failed) std::cerr << "A plugin notification failed.\n";
            },[&](std::string_view command) { return dfy::app::plugin_command(manager,command,std::cout,std::cerr); });
            const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while (!manager.close()) {
                if (manager.close_status() == dfy::plugins::ShutdownResult::failed || std::chrono::steady_clock::now() >= deadline) {
                    std::cerr << "Some plugins could not finish stopping; their resources remain retained.\n"; return 1;
                }
                if (manager.advance() == dfy::runtime::RunResult::failed) {
                    std::cerr << "Plugin stopping failed; its resources remain retained.\n"; return 1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            return result;
        }
        const bool help = args.size() == 2 && args[1] == "--help";
        auto& output = help ? std::cout : std::cerr;
        output << "Usage: dafeiyu [--check-config FILE | --complete CONFIG PROMPT_FILE | --chat CONFIG [--plugins DIR] [--data DIR]]\n"
            << "No arguments: chat with the bundled DeepSeek configuration.\n";
        return help ? 0 : 1;
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
