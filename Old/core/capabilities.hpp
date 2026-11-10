#pragma once
#include "common.hpp"

namespace dfy {
// 取得描述符声明的资源集合，缺省时使用单个主资源。
inline Json resources_of(const Json& descriptor) {
    return descriptor.value("resources", Json::array({descriptor.at("resource")}));
}

// 判断权限集合是否包含指定值或通配授权。
inline bool permits(const Json& values, const Json& value) {
    return includes(values, "*") || includes(values, value);
}

// 计算两个权限集合的交集，并处理通配授权。
inline Json intersect_permissions(const Json& left, const Json& right) {
    if (includes(left, "*"))
        return right;
    if (includes(right, "*"))
        return left;

    Json result = Json::array();
    for (const auto& value : left)
        if (includes(right, value) && !includes(result, value))
            result.push_back(value);
    return result;
}

// 取得显式委托权限，或根据方法副作用和资源生成默认委托。
inline Json delegation_of(const Json& descriptor) {
    Json effects = Json::array({"read"});
    if (descriptor.at("effects") != "read")
        effects.push_back(descriptor.at("effects"));
    return descriptor.value("delegates", Json{{"effects", effects}, {"resources", resources_of(descriptor)}});
}

// 校验方法资源与委托权限声明，阻止委托扩大可写副作用等级。
// 步骤：校验主资源及资源集合 → 检查委托结构与副作用等级 → 校验委托资源名称。
inline void validate_permissions(const Json& descriptor) {
    const auto resources = resources_of(descriptor);
    require(resources.is_array() && !resources.empty() && includes(resources, descriptor.at("resource")),
        "METHOD_RESOURCES_INVALID");
    for (const auto& item : resources)
        require(item.is_string() && !item.get<std::string>().empty() && item != "*", "METHOD_RESOURCES_INVALID");

    const auto delegation = delegation_of(descriptor);
    require(delegation.at("effects").is_array() && delegation.at("resources").is_array(), "DELEGATION_INVALID");
    for (const auto& item : delegation.at("effects")) {
        require(includes(Json::array({"read", "local-write", "external-write"}), item), "DELEGATION_EFFECT_INVALID");
        if (descriptor.at("effects") == "read")
            require(item == "read", "READ_DELEGATION_WRITES");
        if (descriptor.at("effects") == "local-write")
            require(item != "external-write", "LOCAL_DELEGATION_EXTERNAL_WRITE");
    }
    for (const auto& item : delegation.at("resources"))
        require(item.is_string() && !item.get<std::string>().empty(), "DELEGATION_RESOURCE_INVALID");
}
}
