#include "conversations.hpp"
namespace dfy {
void Conversations::start() {
    std::set<std::string> current, retired_services;
    for (const auto& service : plugins_.channels()) current.insert(plugins_.channel_binding(service.at("id")).at("instance").get<std::string>());
    for (auto saved : store_.all("channel_authority")) {
        if (current.contains(saved.at("id").get<std::string>()) || saved.at("policy").value("retired", false)) continue;
        const auto identity = saved.at("policy").at("identity");
        retired_services.insert(identity.at("service").get<std::string>());
        saved["generation"] = saved.at("generation").get<int>() + 1;
        saved["policy"] = {{"retired", true}, {"identity", identity}};
        store_.save("channel_authority", saved.at("id"), saved, "channel.authority_retired", saved);
    }
    for (const auto& service : plugins_.channels()) {
        const auto channel = plugins_.channel_binding(service.at("id"));
        const auto selected = plugins_.selection(channel.at("plugin"));
        const auto policy = Json{{"identity", channel}, {"owner", profile_.data.at("owner")}, {"principals", selected.value("principals", Json::object())},
            {"accept_inputs", selected.value("accept_inputs", false)}, {"allow_reply", selected.at("channels").at(service.at("id").get<std::string>()).value("allow_reply", true)}};
        const auto key = channel.at("instance").get<std::string>();
        auto saved = store_.get("channel_authority", key);
        if (saved.is_null()) saved = {{"id", key}, {"generation", 1}, {"policy", policy}};
        else if (saved.at("policy") != policy || retired_services.contains(service.at("id").get<std::string>())) {
            saved["generation"] = saved.at("generation").get<int>() + 1; saved["policy"] = policy;
        }
        store_.save("channel_authority", key, saved, "channel.authority_loaded", saved);
    }
}
Json Conversations::bind(const std::string& service, const std::string& target, Json authorization, const Json& reply_reference) {
    require(!target.empty() && target.size() <= 2048, "CONVERSATION_TARGET_INVALID");
    const auto channel = plugins_.channel_binding(service);
    const auto policy = store_.get("channel_authority", channel.at("instance"));
    require(!policy.is_null(), "CHANNEL_AUTHORITY_UNAVAILABLE");
    authorization["generation"] = policy.at("generation");
    sdk::ConversationKey key{store_.instance_id(), channel.at("instance"), channel.at("account"), target};
    sdk::DeliveryBinding delivery{channel, authorization, target, reply_reference};
    return {{"id", key.id()}, {"key", key.to_json()}, {"delivery", delivery.to_json()}};
}
Json Conversations::local(const std::string& target, const std::string& service) {
    return bind(service.empty() ? plugins_.binding("channel") : service, target,
        {{"kind", "local-owner"}, {"actor", profile_.data.at("owner")}}, nullptr);
}
Json Conversations::channel(const std::string& plugin, const std::string& service, const Json& input) {
    require(plugins_.owner(service) == plugin && plugins_.service(service).at("contract") == "dafeiyu.channel", "INPUT_CHANNEL_OWNER_DENIED");
    const auto selected = plugins_.selection(plugin);
    require(selected.value("accept_inputs", false), "INPUT_SOURCE_DENIED");
    const auto principal = input.at("actor").get<std::string>();
    require(selected.at("principals").contains(principal) && selected.at("principals").at(principal) == profile_.data.at("owner"), "INPUT_PRINCIPAL_DENIED");
    return bind(service, input.at("conversation"), {{"kind", "channel"}, {"principal", principal}, {"actor", profile_.data.at("owner")}},
        input.value("reply_reference", Json{}));
}
Json Conversations::activity(const std::string& id, const std::string& target, const std::string& reply_service) {
    if (!reply_service.empty()) {
        const auto activity = store_.get("activity", id);
        return bind(reply_service, target, {{"kind", "activity"}, {"actor", profile_.data.at("owner")}, {"activity", id},
            {"definition", fingerprint(activity.at("definition").dump())}}, nullptr);
    }
    sdk::ConversationKey key{store_.instance_id(), "internal:" + id, "internal", target};
    return {{"id", key.id()}, {"key", key.to_json()}, {"delivery", nullptr}};
}
void Conversations::validate_identity(const Json& delivery) const {
    require(delivery.is_object() && delivery.at("channel") == plugins_.channel_binding(delivery.at("channel").at("service")), "DELIVERY_BINDING_CHANGED");
}
void Conversations::authorize(const Json& delivery) {
    if (delivery.is_null()) return;
    validate_identity(delivery);
    const auto& authorization = delivery.at("authorization");
    const auto saved = store_.get("channel_authority", delivery.at("channel").at("instance"));
    require(!saved.is_null() && saved.at("generation") == authorization.at("generation"), "DELIVERY_AUTHORITY_CHANGED");
    require(!saved.at("policy").value("retired", false), "DELIVERY_AUTHORITY_CHANGED");
    require(saved.at("policy").at("allow_reply") == true && authorization.at("actor") == profile_.data.at("owner"), "DELIVERY_AUTHORITY_DENIED");
    if (authorization.at("kind") == "channel") {
        require(saved.at("policy").at("accept_inputs") == true, "DELIVERY_AUTHORITY_DENIED");
        const auto principal = authorization.at("principal").get<std::string>();
        require(saved.at("policy").at("principals").contains(principal) && saved.at("policy").at("principals").at(principal) == authorization.at("actor"), "DELIVERY_AUTHORITY_DENIED");
    } else if (authorization.at("kind") == "activity") {
        const auto activity = store_.get("activity", authorization.at("activity"));
        require(!activity.is_null() && activity.at("enabled") == true && fingerprint(activity.at("definition").dump()) == authorization.at("definition").get<std::string>(), "DELIVERY_AUTHORITY_CHANGED");
    } else require(authorization.at("kind") == "local-owner", "DELIVERY_AUTHORITY_DENIED");
}
}
