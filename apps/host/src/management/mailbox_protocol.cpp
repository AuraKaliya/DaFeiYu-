#include "mailbox_protocol.hpp"
#include "host.hpp"
#include "mailbox_view.hpp"
#include <nlohmann/json.hpp>

namespace dfy::management {
// 从一次不可变采样返回当前实例的公开来源，区分初始读取和旧实例。
// 步骤：校验请求与采样 → 绑定原后台 → 编码当前来源或返回失效目录。
nlohmann::json mailbox_request(const Host& host, const nlohmann::json& request) {
    using Json = nlohmann::json;
    const bool bound = request.contains("instance");
    if (request.at("type")!="mailbox.list" || request.size()!=(bound ? 4 : 3) ||
        (bound && (!request.at("instance").is_string() || request.at("instance").get_ref<const std::string&>().empty())))
        throw std::invalid_argument("mailbox: invalid directory request");
    const auto directory = host.mailbox_directory();
    if (!directory) throw std::runtime_error("mailbox: directory unavailable");
    const bool stale = bound && request.at("instance")!=host.instance();

    auto sources = Json::array();
    if (!stale) for (const auto& [id,source] : directory->sources) {
        Json row{{"id",id},{"label",source.label},{"kind",source.plugin.empty() ? "builtin" : "plugin"},
            {"generation",std::to_string(source.generation)}};
        if (!source.plugin.empty()) row["plugin"] = source.plugin;
        sources.push_back(std::move(row));
    }
    return {{"version","1"},{"request",request.at("request")},{"event","mailbox.directory"},{"instance",host.instance()},
        {"revision",std::to_string(directory->revision)},{"sampledUtcMs",directory->sampled_utc_ms},
        {"state",stale ? "stale" : "ready"},{"sources",std::move(sources)}};
}
}
