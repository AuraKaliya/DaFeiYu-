#include "application.hpp"
#include "companion_host.hpp"
#include "diagnostic_files.hpp"
#include "diagnostics.hpp"
#include "resident_entry.hpp"
#include "software_log.hpp"
#include "management/resident.hpp"
#include "management/client.hpp"
#include "management/terminal_client.hpp"
#include "management/local_pipe.hpp"
#include "trace_commands.hpp"
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
    std::shared_ptr<dfy::app::SoftwareLog> software;
    try {
#ifdef _WIN32
        const dfy::app::Utf8Console console;
#endif

        if (args.size() == 3 && args[1] == "--review-diagnostic") {
            dfy::app::write_diagnostic_review(std::cout,dfy::app::read_diagnostic_file(args[2]));
            return 0;
        }
        if ((args.size() == 3 || args.size() == 5) && args[1] == "--review-trace") {
            std::optional<dfy::agent::RunId> selected;
            if (args.size() == 5) selected = dfy::agent::RunId{dfy::app::trace_id(args[3]),dfy::app::trace_id(args[4])};
            dfy::app::review_trace(args[2],selected,std::cout);
            return 0;
        }
        if (args.size() == 5 && args[1] == "--replay-trace") {
            dfy::app::replay_trace(args[2],{dfy::app::trace_id(args[3]),dfy::app::trace_id(args[4])},std::cout);
            return 0;
        }
        if (args.size() == 3 && args[1] == "--check-config") {
            const auto config = dfy::model::load_config(args[2]);
            std::cout << "Configuration valid: " << config.provider << " / " << config.model
                      << "\nCredentials and service availability have not been checked.\n";
            return 0;
        }
        if (args.size() == 5 && args[1] == "--mc-partner") {
            const auto& root = args[4];
            dfy::management::ProfileLease lease(root/"user");
            dfy::app::ApplicationRunConfig run;
            run.trace_directory = root;
            dfy::app::Application application(dfy::model::load_config(args[2]), root / "packages", root / "user",
                dfy::model::Backend{dfy::model::complete_response},std::move(run));
            dfy::app::run_companion_host(application, args[3], root, dfy::app::CompanionMode::partner);
            return 0;
        }
        if (args.size() == 4 && args[1] == "--complete") {
            const auto config = dfy::model::load_config(args[2]);
            const auto prompt = read_prompt(args[3]);
            std::cout << dfy::model::complete(config, prompt) << '\n';
            return 0;
        }
        const auto options = std::span(args).subspan(1);
        if (!options.empty() && (options[0] == "--host" || options[0] == "--host-status" || options[0] == "--stop-host"))
            return dfy::management::resident_command(options);

        const bool default_chat = options.empty() || options[0] == "--data" || options[0] == "--plugins" || options[0] == "--trace";
        if (default_chat || (options.size() >= 2 && options[0] == "--chat")) {
            const auto paths = default_chat ? dfy::management::resident_paths(std::nullopt, options) :
                dfy::management::resident_paths(options[1], options.subspan(2));
            software = std::make_shared<dfy::app::SoftwareLog>(paths.data, dfy::app::SoftwareRole::terminal);
            (void)dfy::management::read_runtime_config(paths.model, *software);
            const auto identity = dfy::management::startup_identity(paths.model, paths.plugins, paths.trace);
            auto binding = dfy::management::connect_or_launch({}, paths.data, identity, [&] {
                dfy::management::launch_host(paths.model, paths.plugins, paths.data, paths.trace);
            }, software.get());
            software->bind(binding.instance,binding.client);
            software->record(dfy::app::SoftwareEvent::client_connected,dfy::app::SoftwareResult::ok);
            dfy::platform::LineInput input;
            return software->exiting(dfy::management::resident_chat(paths.data,binding,input,std::cout,std::cerr,software.get()));
        }
        const bool help = args.size() == 2 && args[1] == "--help";
        auto& output = help ? std::cout : std::cerr;
        output << "Usage: dafeiyu-cli [--check-config FILE | --complete CONFIG PROMPT_FILE | --chat CONFIG [--plugins DIR] [--data DIR] [--trace DIR]"
            << " | --host CONFIG [--plugins DIR] [--data DIR] [--trace DIR] | --host-status [DATA] | --stop-host [DATA [INSTANCE]]"
            << " | --review-diagnostic FILE | --review-trace FILE [SESSION RUN] | --replay-trace FILE SESSION RUN | --mc-partner CONFIG PACKAGE RUN_DIR]\n"
            << "Default chat uses the bundled DeepSeek configuration and accepts --data, --plugins and --trace.\n"
            << "Chat connects to the resident for this data directory. Use /exit to leave the terminal or --stop-host to stop the resident.\n"
            << "In chat, use /run PACKAGE/TOOL \"GOAL\" to grant one read-only tool.\n"
            << "Use /run-confirmed for state changes, then /confirmations and /approve or /deny REQUEST REVISION.\n"
            << "Add --workspace \"DIRECTORY\" to /run or /run-confirmed to include that project's local environment.\n"
            << "Use /task READ_TOOL CHANGE_TOOL EXPECTED \"GOAL\" [--request ID] for a verified state change.\n"
            << "Use /verification or /task-status REQUEST to inspect evidence and retained status.\n"
            << "Use /diagnostic export \"FILE\" to save a local diagnostic.\n";
        return help ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return software ? software->exiting(1) : 1;
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
