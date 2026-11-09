#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <string_view>

namespace dfy::sdk {
using Json = nlohmann::json;
inline std::string path_text(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}
inline std::filesystem::path path_from_utf8(std::string_view value) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
inline Json error(std::string code, std::string category = "protocol", std::string effects = "none", bool retryable = false) {
    return {{"code", code}, {"category", category}, {"effects", effects}, {"retryable", retryable}, {"message", code}};
}
inline Json outcome(Json data, std::string effects = "none", std::string status = "succeeded") {
    return {{"status", status}, {"effects", effects}, {"coverage", "complete"}, {"data", std::move(data)},
        {"sources", Json::array()}, {"receipts", Json::array()}};
}
}
