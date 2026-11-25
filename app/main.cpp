#include "application.hpp"
#include "companion_host.hpp"
#include "diagnostic_files.hpp"
#include "diagnostics.hpp"
#include "paths.hpp"
#include "software_log.hpp"
#include "management/resident.hpp"
#include "management/client.hpp"
#include "management/terminal_client.hpp"
#include "management/local_pipe.hpp"
#include <nlohmann/json.hpp>
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
// 在正式进程保留配置读取的结果，文件名、配置正文和异常文本不进入软件日志。
dfy::model::Config runtime_config(const std::filesystem::path& file, dfy::app::SoftwareLog& log) {
    try {
        auto config = dfy::model::load_config(file);
        log.record(dfy::app::SoftwareEvent::configuration_read,dfy::app::SoftwareResult::ok);
        return config;
    } catch (...) {
        log.record(dfy::app::SoftwareEvent::configuration_read,dfy::app::SoftwareResult::failed);
        throw;
    }
}

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
        const bool status_query = args.size()>=2 && args.size()<=3 && args[1]=="--host-status";
        const bool stop_request = args.size()>=2 && args.size()<=4 && args[1]=="--stop-host";
        if (status_query || stop_request) {
            const auto data = args.size()>=3 ? args[2] : dfy::app::default_user_data();
            nlohmann::json status;
            if (stop_request) {
                software = std::make_shared<dfy::app::SoftwareLog>(data,dfy::app::SoftwareRole::terminal);
                const auto instance = args.size()==4 ? args[3].string() :
                    dfy::management::resident_request(data,{{"op","status"}}).at("instance").get<std::string>();
                status = dfy::management::request_stop(data,instance,{},software.get());
            } else status = dfy::management::resident_request(data,{{"op","status"}});
            std::cout << status.dump() << '\n';
            return software ? software->exiting(0) : 0;
        }
        const bool default_chat = args.size()==1 || (args.size()>1 && (args[1]=="--data" || args[1]=="--plugins" || args[1]=="--trace"));
        if (default_chat || (args.size() >= 3 && (args[1] == "--chat" || args[1] == "--host"))) {
            const auto configuration = default_chat ? dfy::app::executable_directory()/"config"/"deepseek.example.json" : args[2];
            std::filesystem::path plugins, user_data;
            dfy::app::ApplicationRunConfig run;
            for (std::size_t index = default_chat ? 1 : 3; index < args.size(); index += 2) {
                if (index+1 >= args.size() || args[index+1].empty()) throw std::invalid_argument("app: option path required");
                if (args[index] == "--plugins" && plugins.empty()) plugins = args[index+1];
                else if (args[index] == "--data" && user_data.empty()) user_data = args[index+1];
                else if (args[index] == "--trace" && run.trace_directory.empty()) run.trace_directory = args[index+1];
                else throw std::invalid_argument("app: unknown or duplicate option");
            }
            if (plugins.empty()) plugins = dfy::app::executable_directory()/"plugins";
            if (user_data.empty()) user_data = dfy::app::default_user_data();
            const bool host = args.size()>=3 && args[1]=="--host";
            software = std::make_shared<dfy::app::SoftwareLog>(user_data,host ? dfy::app::SoftwareRole::host : dfy::app::SoftwareRole::terminal);
            run.software_log = software;
            if (host) {
                const auto identity = dfy::management::startup_identity(configuration,plugins,run.trace_directory);
                const int code = dfy::management::serve_resident(user_data,identity,[=,log=&*software] {
                    return std::make_unique<dfy::app::Application>(runtime_config(configuration,*log),plugins,user_data,
                        dfy::model::Backend{dfy::model::complete_response},run);
                },{},&*software);
                return software->exiting(code);
            }
            (void)runtime_config(configuration,*software);
            const auto identity = dfy::management::startup_identity(configuration,plugins,run.trace_directory);
            auto binding = dfy::management::connect_or_launch({},user_data,identity,[&] {
                dfy::management::launch_host(dfy::app::executable_directory()/"dafeiyu.exe",configuration,plugins,user_data,run.trace_directory);
            },software.get());
            software->bind(binding.instance,binding.client);
            software->record(dfy::app::SoftwareEvent::client_connected,dfy::app::SoftwareResult::ok);
            dfy::platform::LineInput input;
            return software->exiting(dfy::management::resident_chat(user_data,binding,input,std::cout,std::cerr,software.get()));
        }
        const bool help = args.size() == 2 && args[1] == "--help";
        auto& output = help ? std::cout : std::cerr;
        output << "Usage: dafeiyu [--check-config FILE | --complete CONFIG PROMPT_FILE | --chat CONFIG [--plugins DIR] [--data DIR] [--trace DIR]"
            << " | --host CONFIG [--plugins DIR] [--data DIR] [--trace DIR] | --host-status [DATA] | --stop-host [DATA [INSTANCE]]"
            << " | --review-diagnostic FILE | --review-trace FILE [SESSION RUN] | --replay-trace FILE SESSION RUN | --mc-partner CONFIG PACKAGE RUN_DIR]\n"
            << "Default chat uses the bundled DeepSeek configuration and accepts --data, --plugins and --trace.\n"
            << "Chat connects to the resident for this data directory. Use /exit to leave the terminal or --stop-host to stop the resident.\n"
            << "In chat, use /run PACKAGE/TOOL \"GOAL\" to grant one read-only tool.\n"
            << "Use /run-confirmed for state changes, then /confirmations and /approve or /deny REQUEST REVISION.\n"
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
