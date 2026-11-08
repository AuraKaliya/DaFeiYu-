#pragma once
#include <dafeiyu/plugin.hpp>
#include <set>

namespace dfy::sdk {
// ABI v1 supports flat strict objects with integer/string/boolean properties only.
// Reject unsupported schemas at load time instead of pretending to implement all JSON Schema.
inline void validate_action_schema(const Json& schema) {
    const std::set<std::string> keys{"type", "properties", "required", "additionalProperties"};
    for (const auto& [key, value] : schema.items()) { (void)value; if (!keys.contains(key)) throw std::runtime_error("SCHEMA_KEYWORD_UNSUPPORTED"); }
    if (schema.at("type") != "object" || schema.at("additionalProperties") != false ||
        !schema.at("properties").is_object() || !schema.at("required").is_array()) throw std::runtime_error("SCHEMA_INVALID");
    std::set<std::string> required;
    for (const auto& field : schema.at("required")) {
        const auto key = field.get<std::string>();
        if (!schema.at("properties").contains(key) || !required.insert(key).second) throw std::runtime_error("SCHEMA_REQUIRED_INVALID");
    }
    for (const auto& [name, property] : schema.at("properties").items()) {
        (void)name;
        const auto type = property.at("type").get<std::string>();
        if (type != "integer" && type != "string" && type != "boolean") throw std::runtime_error("SCHEMA_TYPE_UNSUPPORTED");
        for (const auto& [key, value] : property.items()) {
            if (key == "type") continue;
            if ((key != "minimum" && key != "maximum") || type != "integer" || !value.is_number_integer()) throw std::runtime_error("SCHEMA_KEYWORD_UNSUPPORTED");
        }
        if (property.contains("minimum") && property.contains("maximum") && property.at("minimum") > property.at("maximum")) throw std::runtime_error("SCHEMA_RANGE_INVALID");
    }
}
inline void validate_action_arguments(const Json& schema, const Json& input) {
    if (!input.is_object()) throw std::runtime_error("ACTION_ARGUMENTS_INVALID");
    for (const auto& required : schema.at("required")) if (!input.contains(required.get<std::string>())) throw std::runtime_error("ACTION_ARGUMENT_REQUIRED");
    for (const auto& [key, value] : input.items()) {
        if (!schema.at("properties").contains(key)) throw std::runtime_error("ACTION_ARGUMENT_UNKNOWN");
        const auto& property = schema.at("properties").at(key);
        const auto type = property.at("type").get<std::string>();
        if ((type == "integer" && !value.is_number_integer()) || (type == "string" && !value.is_string()) ||
            (type == "boolean" && !value.is_boolean())) throw std::runtime_error("ACTION_ARGUMENT_TYPE");
        if (property.contains("minimum") && value < property.at("minimum")) throw std::runtime_error("ACTION_ARGUMENT_RANGE");
        if (property.contains("maximum") && value > property.at("maximum")) throw std::runtime_error("ACTION_ARGUMENT_RANGE");
    }
}
}
