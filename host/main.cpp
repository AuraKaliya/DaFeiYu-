#include <dafeiyu/core.hpp>
#include <atomic>
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
std::atomic<bool> interrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free);
void interrupt_handler(int) { interrupted.store(true, std::memory_order_relaxed); }
int run(const std::vector<std::string>& args) {
    if (args.size() < 2 || args[1] == "--help") {
        std::cout << "dafeiyu <run|resume|status|events|plugins> [--profile FILE] --state DIR\n"
                     "  run --request-id ID [--message TEXT] [--pause-after-action]\n"
                     "  resume/status/events --request-id ID\n";
        return args.size() < 2 ? 2 : 0;
    }
    const auto command = args[1];
    if (command != "run" && command != "resume" && command != "status" && command != "events" && command != "plugins") throw std::runtime_error("UNKNOWN_COMMAND");
    std::map<std::string, std::string> options;
    bool pause = false;
    for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--pause-after-action") { pause = true; continue; }
        if (args[i] != "--profile" && args[i] != "--state" && args[i] != "--request-id" && args[i] != "--message") throw std::runtime_error("UNKNOWN_OPTION");
        if (i + 1 >= args.size() || options.contains(args[i])) throw std::runtime_error("INVALID_OPTION");
        const auto key = args[i++];
        options.emplace(key, args[i]);
    }
    if (!options.contains("--state")) throw std::runtime_error("STATE_DIRECTORY_REQUIRED");
    const auto state = dfy::sdk::path_from_utf8(options.at("--state"));
    const auto request = options.contains("--request-id") ? options.at("--request-id") : std::string{};
    if (command != "plugins" && request.empty()) throw std::runtime_error("REQUEST_ID_REQUIRED");
    dfy::Json result;
    if (command == "status" || command == "events") {
        dfy::Store store(state);
        result = command == "status" ? store.find(request) : store.events(request);
        if (result.is_null()) throw std::runtime_error("REQUEST_NOT_FOUND");
    } else {
        const auto profile = options.contains("--profile") ? dfy::sdk::path_from_utf8(options.at("--profile")) :
            std::filesystem::absolute(dfy::sdk::path_from_utf8(args.front())).parent_path().parent_path() / "profiles" / "demo.json";
        dfy::Runtime runtime(dfy::Profile::open(profile), state);
        std::stop_source cancellation;
        std::signal(SIGINT, interrupt_handler);
        std::jthread monitor([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                if (interrupted.load(std::memory_order_relaxed)) { cancellation.request_stop(); return; }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        dfy::RunOptions run_options;
        run_options.stop = cancellation.get_token();
        run_options.pause_after_action = pause;
        run_options.observer = [](std::string_view type) { std::cerr << "[dafeiyu] " << type << '\n'; };
        if (command == "plugins") result = runtime.plugins();
        else if (command == "resume") result = runtime.resume(request, run_options);
        else result = runtime.submit(request, options.contains("--message") ? options.at("--message") : "请运行示例加法任务。", run_options);
    }
    std::cout << result.dump(2, ' ', false, dfy::Json::error_handler_t::replace) << '\n';
    if (result.is_object() && result.value("execution", std::string{}) == "failed") return 1;
    if (result.is_object() && result.value("execution", std::string{}) == "cancelled") return 3;
    return 0;
}
int entry(const std::vector<std::string>& args) {
    try { return run(args); }
    catch (const std::exception& error) { std::cerr << dfy::Json{{"error", error.what()}}.dump(-1, ' ', false, dfy::Json::error_handler_t::replace) << '\n'; return 1; }
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(dfy::sdk::path_text(std::filesystem::path(argv[i])));
    std::wstring executable(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (size && size < executable.size()) { executable.resize(size); args[0] = dfy::sdk::path_text(std::filesystem::path(executable)); }
    return entry(args);
}
#else
int main(int argc, char** argv) { return entry(std::vector<std::string>(argv, argv + argc)); }
#endif
