#pragma once
#include "common.hpp"
namespace dfy {
inline Json resources_of(const Json& descriptor) {
    return descriptor.value("resources", Json::array({descriptor.at("resource")}));
}
inline bool permits(const Json& values, const Json& value) { return includes(values, "*") || includes(values, value); }
inline Json intersect_permissions(const Json& left, const Json& right) {
    if (includes(left, "*")) return right;
    if (includes(right, "*")) return left;
    Json result = Json::array();
    for (const auto& value : left) if (includes(right, value) && !includes(result, value)) result.push_back(value);
    return result;
}
inline Json delegation_of(const Json& descriptor) {
    Json effects = Json::array({"read"});
    if (descriptor.at("effects") != "read") effects.push_back(descriptor.at("effects"));
    return descriptor.value("delegates", Json{{"effects", effects}, {"resources", resources_of(descriptor)}});
}
inline void validate_permissions(const Json& descriptor) {
    const auto resources = resources_of(descriptor);
    require(resources.is_array() && !resources.empty() && includes(resources, descriptor.at("resource")), "METHOD_RESOURCES_INVALID");
    for (const auto& item : resources) require(item.is_string() && !item.get<std::string>().empty() && item != "*", "METHOD_RESOURCES_INVALID");
    const auto delegation = delegation_of(descriptor);
    require(delegation.at("effects").is_array() && delegation.at("resources").is_array(), "DELEGATION_INVALID");
    for (const auto& item : delegation.at("effects")) {
        require(includes(Json::array({"read", "local-write", "external-write"}), item), "DELEGATION_EFFECT_INVALID");
        if (descriptor.at("effects") == "read") require(item == "read", "READ_DELEGATION_WRITES");
        if (descriptor.at("effects") == "local-write") require(item != "external-write", "LOCAL_DELEGATION_EXTERNAL_WRITE");
    }
    for (const auto& item : delegation.at("resources")) require(item.is_string() && !item.get<std::string>().empty(), "DELEGATION_RESOURCE_INVALID");
}
}
