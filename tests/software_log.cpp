#include "software_log.hpp"
#include "software_log_files.hpp"
#include "application.hpp"
#include <dafeiyu/dynamic_library.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <nlohmann/json.hpp>

namespace {
namespace fs = std::filesystem;
using namespace dfy::app;
using namespace std::chrono_literals;

// 用独占活动段和多次换段核对完整行、两种保留预算及非产品文件的归属。
// 步骤：保留活动段和无关文件 → 让另一写入者反复换段 → 核对收尾后的实际文件与字节。
void retention(const fs::path& folder, SoftwareLogLimits limits) {
    SoftwareLogFiles active(folder,limits);
    const std::string active_line(20,'a'), other_line(20,'b');
    active.append(active_line+"\n");
    const auto original = fs::directory_iterator(folder)->path();
    std::ofstream(folder/"software-notes.jsonl") << "keep";
    fs::create_directory(folder/"software-1-2-3-4.jsonl");

    SoftwareLogFiles other(folder,limits);
    for (int index=0; index<9; ++index) other.append(other_line+"\n");
    other.close();
    std::string value;
    std::ifstream read(original);
    if (!std::getline(read,value) || value != active_line) throw std::runtime_error("active segment lost");
    read.close();
    active.close();

    std::uintmax_t bytes = 0;
    std::size_t files = 0;
    bool found_latest = false;
    for (const auto& file : fs::directory_iterator(folder)) {
        if (!file.is_regular_file() || file.path().filename()=="software-notes.jsonl") continue;
        std::ifstream input(file.path(),std::ios::binary);
        if (!std::getline(input,value) || (value!=active_line && value!=other_line) || input.peek()!=EOF)
            throw std::runtime_error("record split at segment boundary");
        found_latest |= value==other_line;
        bytes += file.file_size();
        ++files;
    }
    if (!files || !found_latest || files>limits.retained_files || bytes>limits.retained_bytes ||
        fs::file_size(folder/"software-notes.jsonl")!=4 || !fs::is_directory(folder/"software-1-2-3-4.jsonl"))
        throw std::runtime_error("retention limits or ownership failed");
}

// 以同名普通文件阻止日志建目录，验证错误可见且保留业务退出码。
void unavailable(const fs::path& profile) {
    fs::create_directory(profile);
    std::ofstream(profile/"logs") << "original";
    SoftwareLog log(profile,SoftwareRole::terminal);
    const auto end = std::chrono::steady_clock::now()+2s;
    while (!log.error() && std::chrono::steady_clock::now()<end) std::this_thread::sleep_for(5ms);
    if (!log.error() || log.exiting(7)!=7 || fs::file_size(profile/"logs")!=8)
        throw std::runtime_error("log failure changed application outcome");
}

// 将合成正文和凭据送入允许的身份字段，检查磁盘只保留格式合规的身份。
// 步骤：提交异常字段和正常退出操作 → 等待写完 → 检查磁盘字段及原始字节。
void private_fields(const fs::path& profile) {
    {
        SoftwareLog log(profile,SoftwareRole::window);
        log.record(SoftwareEvent::client_connected,SoftwareResult::failed,
            {.instance="secret-prompt-marker",.client="client-1-2-3-secret-chat-marker",.shutdown=true});
        log.record(SoftwareEvent::stop_reply,SoftwareResult::accepted,{.instance="1-2-3",.client="client-4-5-6-7",.shutdown=true});
        log.exiting(0);
    }
    std::ifstream input(fs::directory_iterator(profile/"logs")->path(),std::ios::binary);
    bool matched = false;
    for (std::string line; std::getline(input,line); ) {
        if (line.find("secret-")!=std::string::npos) throw std::runtime_error("private text in software log");
        const auto row = nlohmann::json::parse(line);
        if (row.at("event")=="stop.reply") matched = row.at("operation")=="stop-1-2-3" && row.at("client")=="client-4-5-6-7";
        if (row.at("event")=="client.connected" && (row.contains("operation") || row.contains("client") || row.contains("instance")))
            throw std::runtime_error("invalid log identity");
    }
    if (!matched) throw std::runtime_error("missing original stop operation");
}

// 用真实 DLL 分别制造占用拒绝、停止等待和实际卸载，再串查同一实例的记录。
// 步骤：安装可控生命周期包 → 持有并释放代码租约 → 延迟轮询至实退 → 检查原实例及耗时。
void plugin_lifetime(const fs::path& root, const fs::path& fixture) {
    const auto package = root/"packages"/"pending", profile = root/"user";
    fs::create_directories(package);
    fs::copy_file(fixture,package/"entry.dll");
    std::ofstream(package/"plugin.json") << R"({"manifest_version":1,"id":"test.log","name":"fixed","version":"1","abi_version":2,"entry":"entry.dll"})";
    std::uint64_t generation = 0;
    {
        auto log = std::make_shared<SoftwareLog>(profile,SoftwareRole::host);
        log->bind("1-2-3");
        Application application(dfy::model::Config{},root/"packages",profile,dfy::model::Backend{
            [](const auto&, const auto&) -> dfy::model::Response { throw std::logic_error("unexpected model call"); }},
            ApplicationRunConfig{.software_log=log});
        auto& plugins = application.plugins();
        if (plugins.load("test.log")) throw std::runtime_error("unable to load fixture");
        const auto instance = plugins.instance("test.log");
        generation = instance->generation();
        auto pin = instance->retain_code();
        if (plugins.unload("test.log")!=dfy::plugins::ShutdownResult::in_use) throw std::runtime_error("code lease was ignored");
        pin.reset();

        dfy::platform::DynamicLibrary control(package/"entry.dll");
        const auto mode = reinterpret_cast<void (DFY_V2_CALL*)(std::uint32_t)>(control.symbol("dfy_lifecycle_mode"));
        mode(4);
        log->record(SoftwareEvent::stop_accepted,SoftwareResult::accepted,{.shutdown=true});
        if (plugins.unload("test.log")!=dfy::plugins::ShutdownResult::pending) throw std::runtime_error("fixture did not wait");
        std::this_thread::sleep_for(35ms);
        plugins.advance();
        if (!plugins.loaded().empty()) throw std::runtime_error("fixture still loaded");
    }
    std::ifstream input(fs::directory_iterator(profile/"logs")->path());
    std::vector<std::string> stages;
    for (std::string line; std::getline(input,line); ) {
        const auto row = nlohmann::json::parse(line);
        if (!row.contains("pluginInstance")) continue;
        if (row.at("pluginInstance")!=generation) throw std::runtime_error("plugin generation changed");
        stages.push_back(row.at("event").get<std::string>()+"."+row.at("result").get<std::string>());
        if (row.at("event")=="plugin.unloaded" && (row.at("durationMs")<30 || row.at("operation")!="stop-1-2-3"))
            throw std::runtime_error("missing original plugin wait");
    }
    if (stages!=std::vector<std::string>{"plugin.load.ok","plugin.stop.busy","plugin.stop.pending","plugin.unloaded.ok"})
        throw std::runtime_error("plugin unload reported before actual release");
}
}

// 在本次独立测试目录对照按数量、按大小轮转和写入失败。
// 步骤：创建独占场地 → 验证两种预算与故障 → 回收本次文件。
int main(int argc, char** argv) {
    try {
        if (argc!=3) return 2;
        fs::create_directories(argv[1]);
        const auto root = fs::path(argv[1])/(std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
        if (!fs::create_directory(root)) throw std::runtime_error("fixture directory in use");
        retention(root/"count",{32,4096,2});
        retention(root/"bytes",{32,62,64});
        unavailable(root/"failure");
        private_fields(root/"privacy");
        plugin_lifetime(root/"plugin",argv[2]);
        fs::remove_all(root);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
