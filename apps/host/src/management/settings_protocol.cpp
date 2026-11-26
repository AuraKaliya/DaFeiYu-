#include "protocol.hpp"
#include "host.hpp"
#include "host_settings.hpp"
#include <nlohmann/json.hpp>

namespace dfy::management {
// 只公开当前后台已登记的主体配置，旧实例或旧修订须重新取样。
// 步骤：核对观察者与所有者 → 读取一次配置快照 → 区分过期、未配置与可展示的字段。
nlohmann::json settings_request(Host& host, const nlohmann::json& request, const std::string& client) {
    using Json = nlohmann::json;
    const bool pinned = request.contains("revision");
    if (request.at("type")!="settings.read" || request.at("owner")!="host" || request.size()!=5u+pinned)
        throw std::invalid_argument("settings: invalid request");
    if (!host.visit_client(client)) throw std::invalid_argument("settings: management client required");
    const auto& instance = request.at("instance").get_ref<const std::string&>();
    if (instance.empty() || instance.size()>64) throw std::invalid_argument("settings: invalid instance");
    const auto service = host.settings();
    if (!service) throw std::runtime_error("settings: host not ready");

    const auto view = service->snapshot();
    const bool stale = instance!=host.instance() || (pinned && management_number(request.at("revision"))!=view.revision);
    const bool ready = !stale && !view.provider.empty();
    auto fields = Json::array();
    if (ready) {
        fields.push_back({{"id","provider"},{"type","string"},{"access_mode","read_only"},{"source","startup"},
            {"value",view.provider},{"appliedValue",view.provider}});
        fields.push_back({{"id","timeout_ms"},{"type","integer"},{"access_mode","writable"},{"source",view.user_timeout ? "user" : "startup"},
            {"min",1},{"max",600000},{"value",view.timeout_ms},{"appliedValue",view.applied_timeout_ms}});
    }
    return {{"version","1"},{"event","settings.snapshot"},{"request",request.at("request")},{"owner","host"},
        {"instance",host.instance()},{"state",stale ? "stale" : ready ? "ready" : "unavailable"},
        {"revision",std::to_string(view.revision)},{"appliedRevision",std::to_string(view.applied_revision)},
        {"pendingRestart",ready && view.timeout_ms!=view.applied_timeout_ms},{"canSave",ready && view.can_save},{"fields",std::move(fields)}};
}
}
