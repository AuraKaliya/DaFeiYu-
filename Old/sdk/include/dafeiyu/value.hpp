#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <string_view>

namespace dfy::sdk {
using Json = nlohmann::json;

// 将本地文件系统路径编码为 UTF-8 字符串。
inline std::string path_text(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

// 将 UTF-8 字符串转换为本地文件系统路径。
inline std::filesystem::path path_from_utf8(std::string_view value) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

// 构造包含错误码、类别、副作用状态和重试提示的错误记录。
inline Json error(
    std::string code, std::string category = "protocol", std::string effects = "none", bool retryable = false) {
    return {{"code", code}, {"category", category}, {"effects", effects}, {"retryable", retryable}, {"message", code}};
}

// 将业务数据包装为带状态、副作用及来源回执的标准操作结果。
inline Json outcome(Json data, std::string effects = "none", std::string status = "succeeded") {
    return {{"status", status}, {"effects", effects}, {"coverage", "complete"}, {"data", std::move(data)},
        {"sources", Json::array()}, {"receipts", Json::array()}};
}
}
