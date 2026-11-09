#pragma once
#include <dafeiyu/plugin.hpp>
#include <set>

namespace dfy::sdk {
inline void validate_schema(const Json& schema, unsigned depth = 0) {
    if (depth > 16 || !schema.is_object()) throw std::runtime_error("SCHEMA_INVALID");
    if (schema.empty()) return;
    static const std::set<std::string> keys{"type", "properties", "required", "additionalProperties", "items", "enum", "minimum", "maximum", "minItems", "maxItems", "minLength", "maxLength"};
    for (const auto& [key, value] : schema.items()) { (void)value; if (!keys.contains(key)) throw std::runtime_error("SCHEMA_KEYWORD_UNSUPPORTED"); }
    const auto type = schema.at("type").get<std::string>();
    if (!std::set<std::string>{"object", "array", "string", "integer", "number", "boolean", "null"}.contains(type)) throw std::runtime_error("SCHEMA_TYPE_UNSUPPORTED");
    if (schema.contains("enum") && (!schema["enum"].is_array() || schema["enum"].empty())) throw std::runtime_error("SCHEMA_ENUM_INVALID");
    if (type == "object") {
        if (!schema.at("properties").is_object() || !schema.at("additionalProperties").is_boolean()) throw std::runtime_error("SCHEMA_OBJECT_INVALID");
        for (const auto& [name, field] : schema.at("properties").items()) { (void)name; validate_schema(field, depth + 1); }
        std::set<std::string> required;
        for (const auto& name : schema.value("required", Json::array())) {
            if (!name.is_string() || !schema.at("properties").contains(name.get<std::string>()) || !required.insert(name.get<std::string>()).second) throw std::runtime_error("SCHEMA_REQUIRED_INVALID");
        }
    } else if (schema.contains("properties") || schema.contains("required") || schema.contains("additionalProperties")) throw std::runtime_error("SCHEMA_KEYWORD_TYPE");
    if (type == "array") validate_schema(schema.at("items"), depth + 1);
    else if (schema.contains("items")) throw std::runtime_error("SCHEMA_KEYWORD_TYPE");
    for (const auto& key : {"minimum", "maximum", "minItems", "maxItems", "minLength", "maxLength"}) {
        if (!schema.contains(key)) continue;
        const std::string name(key);
        if (name == "minimum" || name == "maximum") {
            if ((type != "integer" && type != "number") || !schema[key].is_number()) throw std::runtime_error("SCHEMA_BOUND_INVALID");
        } else if (!schema[key].is_number_integer() || schema[key].get<int64_t>() < 0 ||
            (name.ends_with("Items") ? type != "array" : type != "string")) throw std::runtime_error("SCHEMA_BOUND_INVALID");
    }
    for (const auto& pair : {std::pair{"minimum", "maximum"}, {"minItems", "maxItems"}, {"minLength", "maxLength"}})
        if (schema.contains(pair.first) && schema.contains(pair.second) && schema[pair.first] > schema[pair.second]) throw std::runtime_error("SCHEMA_RANGE_INVALID");
}
inline void validate_value(const Json& schema, const Json& value, unsigned depth = 0) {
    if (depth > 32) throw std::runtime_error("VALUE_DEPTH_LIMIT");
    if (schema.empty()) return;
    const auto type = schema.at("type").get<std::string>();
    const bool valid = (type == "object" && value.is_object()) || (type == "array" && value.is_array()) ||
        (type == "string" && value.is_string()) || (type == "integer" && value.is_number_integer()) ||
        (type == "number" && value.is_number()) || (type == "boolean" && value.is_boolean()) || (type == "null" && value.is_null());
    if (!valid) throw std::runtime_error("VALUE_TYPE_INVALID");
    if (schema.contains("enum") && std::find(schema["enum"].begin(), schema["enum"].end(), value) == schema["enum"].end()) throw std::runtime_error("VALUE_ENUM_INVALID");
    if (type == "object") {
        for (const auto& field : schema.value("required", Json::array())) if (!value.contains(field.get<std::string>())) throw std::runtime_error("VALUE_REQUIRED");
        for (const auto& [key, field] : value.items()) {
            if (schema.at("properties").contains(key)) validate_value(schema.at("properties").at(key), field, depth + 1);
            else if (!schema.at("additionalProperties").get<bool>()) throw std::runtime_error("VALUE_UNKNOWN_FIELD");
        }
    }
    if (type == "array") for (const auto& item : value) validate_value(schema.at("items"), item, depth + 1);
    for (const auto& bound : {"minimum", "maximum"}) if (schema.contains(bound)) {
        if (std::string_view(bound) == "minimum" ? value < schema[bound] : value > schema[bound]) throw std::runtime_error("VALUE_RANGE_INVALID");
    }
    if (type == "array" || type == "string") {
        size_t size = value.size();
        if (type == "string") {
            size = 0;
            for (const unsigned char ch : value.get_ref<const std::string&>()) if ((ch & 0xc0) != 0x80) ++size;
        }
        const auto low = type == "array" ? "minItems" : "minLength", high = type == "array" ? "maxItems" : "maxLength";
        if ((schema.contains(low) && size < schema[low].get<size_t>()) || (schema.contains(high) && size > schema[high].get<size_t>())) throw std::runtime_error("VALUE_SIZE_INVALID");
    }
}
}
