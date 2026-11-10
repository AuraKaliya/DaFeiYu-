#include "control.hpp"
#include <csignal>
#include <iostream>
#include <map>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace {
std::atomic<bool> interrupted{};

// 将进程中断信号转换为宿主停止标记。
void interrupt(int) {
    interrupted.store(true);
}

// 解析命令行并启动常驻服务、发送控制命令或执行示例任务。
// 步骤：解析动作和选项 → 按模式发送请求或加载实例 → 提供服务或等待示例终态 → 输出结果并转换退出码。
int entry(const std::vector<std::string>& args) {
    try {
        if (args.size() < 2 || args[1] == "--help") {
            std::cout << "dafeiyu serve --state DIR [--profile FILE]\n"
                         "dafeiyu request COMMAND --state DIR [--json JSON] [--command-id ID]\n"
                         "dafeiyu demo --state DIR [--profile FILE] [--message TEXT]\n";
            return 0;
        }

        std::map<std::string, std::string> options;
        size_t begin = args[1] == "request" ? 3 : 2;
        if (args[1] == "request" && args.size() < 3)
            throw std::runtime_error("COMMAND_REQUIRED");
        for (size_t i = begin; i < args.size(); i += 2) {
            if (i + 1 == args.size() || options.contains(args[i]))
                throw std::runtime_error("ARGUMENT_INVALID");
            options.emplace(args[i], args[i + 1]);
        }

        if (!options.contains("--state"))
            throw std::runtime_error("STATE_REQUIRED");
        const auto state = dfy::sdk::path_from_utf8(options.at("--state"));

        if (args[1] == "request") {
            const auto input =
                options.contains("--json") ? dfy::Json::parse(options.at("--json")) : dfy::Json::object();
            const auto result = dfy::host::send(
                state, args[2], input, options.contains("--command-id") ? options.at("--command-id") : "");
            std::cout << result.dump(2) << '\n';
            return 0;
        }

        const auto profile_path =
            options.contains("--profile")
                ? dfy::sdk::path_from_utf8(options.at("--profile"))
                : std::filesystem::absolute(dfy::sdk::path_from_utf8(args.front())).parent_path().parent_path() /
                      "profiles" / "minimal.json";
        if (!std::filesystem::exists(profile_path))
            throw std::runtime_error("PROFILE_REQUIRED: use --profile FILE or build with DFY_BUILD_EXAMPLES=ON");
        dfy::InstanceRuntime runtime(dfy::Profile::open(profile_path), state);

        if (args[1] == "serve") {
            std::signal(SIGINT, interrupt);
            std::signal(SIGTERM, interrupt);
            std::cerr << "[dafeiyu] instance online\n";
            dfy::host::serve(runtime, state, interrupted);
            return 0;
        }

        if (args[1] != "demo")
            throw std::runtime_error("COMMAND_UNKNOWN");
        const auto key = std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
        const auto input = runtime.request("input.accept",
            {{"key", key}, {"text", options.contains("--message") ? options.at("--message") : "运行示例加法任务"}});

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline) {
            const auto receipt = runtime.request("input.get", {{"id", input.at("id")}});
            if (!receipt.at("run_id").is_null()) {
                const auto run = runtime.request("run.get", {{"id", receipt.at("run_id")}});
                if (run.at("execution") == "completed" || run.at("execution") == "failed" ||
                    run.at("execution") == "cancelled") {
                    dfy::Json summary;
                    for (const auto* field : {"id", "execution", "phase", "delivery", "goal", "answer", "results",
                             "error", "model_attempts", "strategy_steps"})
                        if (run.contains(field))
                            summary[field] = run.at(field);
                    std::cout << summary.dump(2) << '\n';
                    return run.at("execution") == "completed" ? 0 : 1;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("DEMO_TIMEOUT");
    } catch (const std::exception& error) {
        std::cerr << dfy::Json{{"error", error.what()}}.dump() << '\n';
        return 1;
    }
}
}
#ifdef _WIN32
// 设置 Windows 控制台编码，将宽字符参数转换后交给统一入口。
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;

    for (int i = 0; i < argc; ++i)
        args.push_back(dfy::sdk::path_text(std::filesystem::path(argv[i])));
    return entry(args);
}
#else
// 将进程参数交给统一命令行入口处理。
int main(int argc, char** argv) {
    return entry(std::vector<std::string>(argv, argv + argc));
}
#endif
