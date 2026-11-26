#include "protocol.hpp"
#include "host.hpp"
#include "host_settings.hpp"
#include "plugin_directory.hpp"
#include <nlohmann/json.hpp>

namespace dfy::management {
namespace {
using Json = nlohmann::json;
// 从原后台已发布的包行读取同代次的公开配置，仅有效活动实例可以返回字段。
// 步骤：核对命令与观察身份 → 匹配包及代次 → 编码只读字段或明确的披露状态。
Json read_plugin_settings(Host& host,const Json& request,const std::string& client) {
    const bool pinned=request.contains("revision");
    if (request.at("type")!="settings.read" || request.size()!=7u+pinned) throw std::invalid_argument("settings: invalid plugin read");
    if (!host.visit_client(client)) throw std::invalid_argument("settings: management client required");
    const auto& instance=plugin_identity(request.at("instance"),64);
    const auto& id=plugin_identity(request.at("id"),64);
    const auto generation=management_number(request.at("generation"));
    const auto revision=pinned ? management_number(request.at("revision")) : generation;
    const auto directory=host.packages();
    if (!directory) throw std::runtime_error("settings: plugin directory unavailable");
    auto fields=Json::array();
    const auto row=directory->packages.find(id);
    std::string_view state="stale";
    if (instance==host.instance() && generation && generation==revision && row!=directory->packages.end() && row->second.generation==generation) {
        const auto& settings=row->second.public_settings;
        if (row->second.state!="active" || settings.state==plugins::PublicSettingsState::unavailable) state="unavailable";
        else if (settings.state==plugins::PublicSettingsState::not_declared) state="notDeclared";
        else {
            state="ready";
            for (const auto& field : settings.fields) fields.push_back({{"id",field.id},{"type","string"},{"access_mode","read_only"},
                {"source",field.user_override ? "user" : "default"},{"value",field.value},{"appliedValue",field.value}});
        }
    }
    Json reply{{"version","1"},{"event","settings.snapshot"},{"request",request.at("request")},{"owner","plugin"},
        {"instance",host.instance()},{"id",id},{"generation",std::to_string(generation)},{"state",state},{"canSave",false},{"fields",std::move(fields)}};
    if (state!="stale") reply.update(Json{{"revision",std::to_string(generation)},{"appliedRevision",std::to_string(generation)}});
    return reply;
}

// 只投影登记字段，区分已保存、启动采用及需要重启的真实状态。
Json read_settings(const app::HostSettingsView& view, bool stale, Json reply) {
    const bool ready = !stale && view.available && !view.provider.empty();
    auto fields = Json::array();
    if (ready) {
        fields.push_back({{"id","provider"},{"type","string"},{"access_mode","read_only"},{"source","startup"},
            {"value",view.provider},{"appliedValue",view.provider}});
        fields.push_back({{"id","timeout_ms"},{"type","integer"},{"access_mode","writable"},{"source",view.user_timeout ? "user" : "startup"},
            {"min",app::host_timeout_min},{"max",app::host_timeout_max},{"value",view.timeout_ms},{"appliedValue",view.applied_timeout_ms}});
    }
    reply.update(Json{{"event","settings.snapshot"},{"state",stale ? "stale" : ready ? "ready" : "unavailable"},
        {"revision",std::to_string(view.revision)},{"appliedRevision",std::to_string(view.applied_revision)},
        {"pendingRestart",ready && view.timeout_ms!=view.applied_timeout_ms},{"canSave",ready && view.can_save},{"fields",std::move(fields)}});
    return reply;
}

// 按登记的字段、类型和边界整批裁定，只有有效的超时才进入原子保存。
app::HostSaveResult save_settings(app::HostSettings& service, const Json& request, const std::string& submission) {
    const auto base = management_number(request.at("revision"));
    const auto& changes = request.at("changes");
    if (changes.is_object() && changes.contains("provider")) return {"readOnly",0,service.snapshot()};
    if (!changes.is_object() || changes.size()!=1 || !changes.contains("timeout_ms")) return {"invalid",0,service.snapshot()};
    const auto& value = changes.at("timeout_ms");
    if (!value.is_number_integer() || value<app::host_timeout_min || value>app::host_timeout_max) return {"invalid",0,service.snapshot()};
    return service.save(submission,base,value.get<int>());
}
}

// 按当前后台处理主体设置的读取、原子保存和原提交查证。
// 步骤：核对客户端及请求形状 → 校验所有者和实例 → 执行所属操作 → 返回原修订及当前实效。
Json settings_request(Host& host, const Json& request, const std::string& client) {
    if (request.at("owner")=="plugin") return read_plugin_settings(host,request,client);
    const auto& type = request.at("type");
    const bool read = type=="settings.read", save = type=="settings.save", pinned = request.contains("revision");
    if ((!read && !save && type!="settings.receipt") || request.at("owner")!="host" || request.size()!=(read ? 5u+pinned : save ? 8u : 6u))
        throw std::invalid_argument("settings: invalid request");
    const auto& instance = request.at("instance").get_ref<const std::string&>();
    if (instance.empty() || instance.size()>64) throw std::invalid_argument("settings: invalid instance");
    const auto service = host.settings(client,save);
    if (!service) throw std::runtime_error("settings: host unavailable");
    Json reply{{"version","1"},{"request",request.at("request")},{"owner","host"},{"instance",host.instance()}};
    const bool stale = instance!=host.instance();
    if (read) {
        const auto view = service->snapshot();
        return read_settings(view,stale || (pinned && management_number(request.at("revision"))!=view.revision),std::move(reply));
    }
    const auto& submission = request.at("submission").get_ref<const std::string&>();
    if (!app::valid_settings_submission(submission)) throw std::invalid_argument("settings: invalid submission");
    const auto result = stale ? app::HostSaveResult{"stale",0,service->snapshot()} :
        save ? save_settings(*service,request,submission) : service->receipt(submission);
    reply.update(Json{{"event","settings.result"},{"submission",submission},{"state",result.state},{"revision",std::to_string(result.current.revision)},
        {"appliedRevision",std::to_string(result.current.applied_revision)},{"pendingRestart",!stale && result.current.timeout_ms!=result.current.applied_timeout_ms}});
    if (result.saved_revision) {
        reply["savedRevision"]=std::to_string(result.saved_revision);
        reply["savedValue"]=result.saved_timeout_ms;
    }
    return reply;
}
}
