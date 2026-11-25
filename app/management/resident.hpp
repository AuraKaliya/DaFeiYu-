#pragma once
#include "host.hpp"
#include "resident_watch.hpp"
#include "../software_log.hpp"
#include <nlohmann/json_fwd.hpp>

namespace dfy::management {
// 由常驻本体持有的托盘寿命，原生实现自行管理消息线程。
struct ResidentTray {
    // 在业务线程实际收尾后释放图标并等待托盘线程离开。
    virtual ~ResidentTray() = default;
};
// 首次就绪时启动托盘并立即返回；无桌面可返回空值。异步菜单须复制原目录和实例号。
using StartResidentTray = std::function<std::unique_ptr<ResidentTray>(const std::filesystem::path& profile,
    const std::string& instance, app::SoftwareLog* log)>;
struct ResidentConnection { std::string instance, client; std::shared_ptr<ResidentWatch> watch; };
// 在原管道交换仍存活时绑定实例、客户端与可跨越后台退出的观察句柄。
ResidentConnection connect_resident(const std::filesystem::path& profile, const std::string& definition);
// 将有效启动目录和配置路径编码为连接双方可核对的启动标识。
std::string startup_identity(const std::filesystem::path& model, const std::filesystem::path& packages,
    const std::filesystem::path& trace = {});
// 独占目录端点并运行本体，客户端离开后继续推进，显式关闭后返回实际退出结果。
int serve_resident(const std::filesystem::path& profile, const std::string& identity, Host::Open open,
    std::stop_token stop = {}, app::SoftwareLog* log = nullptr, StartResidentTray start_tray = {});
// 通过本地通道发送受版本约束的请求，拒绝、断连及超时以异常返回。
nlohmann::json resident_request(const std::filesystem::path& profile, nlohmann::json request);
// 按原实例申请退出，将提交和实际 IPC 回执分别关联到同一个退出操作。
nlohmann::json request_stop(const std::filesystem::path& profile, const std::string& instance,
    std::string_view client, app::SoftwareLog* log);
}
