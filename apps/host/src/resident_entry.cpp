#include "resident_entry.hpp"
#include "application.hpp"
#include "management/resident.hpp"
#include "paths.hpp"
#include "resident_tray.hpp"
#include <dafeiyu/model_client.hpp>
#include <iostream>

namespace dfy::management {
// 为桌面后台和终端确定同一组配置、包目录和数据目录。
// 步骤：解析各路径并拒绝重复 → 补齐程序资源及用户目录。
ResidentPaths resident_paths(std::optional<std::filesystem::path> model, std::span<const std::filesystem::path> options) {
    if (model && model->empty()) throw std::invalid_argument("app: model configuration path required");
    ResidentPaths paths{model ? *model : app::executable_directory()/"config"/"deepseek.example.json", {}, {}, {}};
    for (std::size_t i = 0; i < options.size(); i += 2) {
        if (i + 1 >= options.size() || options[i+1].empty()) throw std::invalid_argument("app: option path required");
        if (options[i] == "--plugins" && paths.plugins.empty()) paths.plugins = options[i+1];
        else if (options[i] == "--data" && paths.data.empty()) paths.data = options[i+1];
        else if (options[i] == "--trace" && paths.trace.empty()) paths.trace = options[i+1];
        else throw std::invalid_argument("app: unknown or duplicate option");
    }
    if (paths.plugins.empty()) paths.plugins = app::executable_directory()/"plugins";
    if (paths.data.empty()) paths.data = app::default_user_data();
    return paths;
}

// 读取配置并只记录结果，异常交给拥有该次启动的进程。
model::Config read_runtime_config(const std::filesystem::path& file, app::SoftwareLog& log) {
    try {
        auto config = model::load_config(file);
        log.record(app::SoftwareEvent::configuration_read, app::SoftwareResult::ok);
        return config;
    } catch (...) {
        log.record(app::SoftwareEvent::configuration_read, app::SoftwareResult::failed);
        throw;
    }
}

// 由后台进程或显式管理命令操作原常驻实例。
// 步骤：区分启动与状态管理 → 绑定数据目录及实例 → 交付回执及实际退出码。
int resident_command(std::span<const std::filesystem::path> args) {
    std::shared_ptr<app::SoftwareLog> log;
    try {
        if (args.size() >= 2 && args[0] == "--host") {
            const auto paths = resident_paths(args[1], args.subspan(2));
            log = std::make_shared<app::SoftwareLog>(paths.data, app::SoftwareRole::host);
            app::ApplicationRunConfig run;
            run.trace_directory = paths.trace;
            run.software_log = log;
            run.manage_host_settings = true;
            const auto identity = startup_identity(paths.model, paths.plugins, paths.trace);
            const int code = serve_resident(paths.data, identity, [=] {
                return std::make_unique<app::Application>(read_runtime_config(paths.model, *log), paths.plugins, paths.data,
                    model::Backend{model::complete_response}, run);
            }, {}, log.get(), ui::resident_tray_factory(identity));
            return log->exiting(code);
        }

        const bool query = !args.empty() && args[0] == "--host-status" && args.size() <= 2;
        const bool stop = !args.empty() && args[0] == "--stop-host" && args.size() <= 3;
        if (!query && !stop) throw std::invalid_argument("Expected --host CONFIG, --host-status [DATA] or --stop-host [DATA [INSTANCE]]");
        const auto data = args.size() >= 2 ? args[1] : app::default_user_data();
        if (query) {
            std::cout << resident_request(data, {{"op","status"}}).dump() << '\n';
            return 0;
        }
        log = std::make_shared<app::SoftwareLog>(data, app::SoftwareRole::terminal);
        const auto instance = args.size() == 3 ? args[2].string() :
            resident_request(data, {{"op","status"}}).at("instance").get<std::string>();
        std::cout << request_stop(data, instance, {}, log.get()).dump() << '\n';
        return log->exiting(0);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return log ? log->exiting(1) : 1;
    }
}
}
