#include <dafeiyu/plugin_manifest.hpp>
#include "package_files.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <limits>
#include <regex>
#include <set>
#include <stdexcept>

namespace dfy::plugins {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;

// 提取有长度限制的非空 UTF-8 文本，拒绝纯空格正文及 ASCII 控制字符。
std::string text(const Json& object, const char* field, std::size_t limit) {
    const auto item = object.find(field);
    if (item == object.end() || !item->is_string()) {
        throw std::runtime_error(std::string(field) + ": required string");
    }
    auto value = item->get<std::string>();
    if (value.empty() || value.size() > limit || value.find_first_not_of(' ') == std::string::npos ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; })) {
        throw std::runtime_error(std::string(field) + ": invalid text");
    }
    return value;
}

// 判断路径分量是否避开 Windows 保留设备名、特殊字符和尾部别名规则。
bool safe_name(const std::string& value) {
    static const std::regex reserved(R"((con|prn|aux|nul|conin\$|conout\$|(com|lpt)([1-9]|¹|²|³))(\..*)?)", std::regex::icase);
    return !value.empty() && value.back() != '.' && value.back() != ' ' &&
           value.find_first_of("\\/:*?\"<>|") == std::string::npos && !std::regex_match(value, reserved);
}

// 检查包标识可在清单、依赖图和 Windows 目录中一致使用。
bool package_id(const std::string& value) {
    static const std::regex grammar(R"([a-z][a-z0-9]*([._-][a-z0-9]+)*)");
    return value.size() <= 64 && std::regex_match(value,grammar) && safe_name(value);
}

// 复制有界的直接依赖列表，保留原声明顺序以便诊断。
// 步骤：识别可选数组 → 验证每个包标识及唯一性 → 返回有界声明。
std::vector<std::string> read_dependencies(const Json& object) {
    const auto items = object.find("dependencies");
    if (items == object.end()) return {};
    if (!items->is_array() || items->size() > 64) throw std::runtime_error("dependencies: expected at most 64 package IDs");

    std::vector<std::string> result;
    for (const auto& item : *items) {
        if (!item.is_string()) throw std::runtime_error("dependencies: expected package ID string");
        const auto& id = item.get_ref<const std::string&>();
        if (!package_id(id) || std::ranges::find(result,id) != result.end())
            throw std::runtime_error("dependencies: invalid or duplicate package ID");
        result.push_back(id);
    }
    return result;
}

// 校验包内相对文件路径，逐段拒绝链接并确认最终文件仍位于包内。
// 步骤：检查路径形式 → 核对路径分量及文件类型 → 规范化并检查包边界。
fs::path package_file(const fs::path& root, const std::string& value, const char* field) {
    const auto reject = [field] { throw std::runtime_error(std::string(field) + ": expected file inside package without links"); };
    if (value.empty() || value.front() == '/' || value.back() == '/' || value.find("//") != std::string::npos) {
        reject();
    }
    auto path = root;
    for (const auto& part : fs::path(std::u8string(value.begin(), value.end()))) {
        const auto bytes = part.u8string();
        if (!safe_name(std::string(bytes.begin(), bytes.end()))) {
            reject();
        }
        path /= part;
        if (files::is_reparse_point(path)) reject();
        const auto status = fs::symlink_status(path);
        if (!fs::is_regular_file(status) && !fs::is_directory(status)) {
            reject();
        }
    }

    const auto resolved = fs::canonical(path);
    const auto relative = resolved.lexically_relative(root);
    if (!fs::is_regular_file(path) || relative.empty() || relative.has_root_path() || *relative.begin() == "..") {
        reject();
    }
    return resolved;
}
}

// 离线读取插件描述与文件位置，不执行入口代码，也不解析默认配置内容。
// 步骤：确认目录并读取清单 → 校验字段与版本 → 提取标识和依赖 → 核对入口及配置路径。
Manifest load_manifest(const std::filesystem::path& directory) {
    try {
        auto location = fs::absolute(directory).lexically_normal();
        if (location.has_relative_path() && location.filename().empty()) {
            location = location.parent_path();
        }
        if (files::is_reparse_point(location) || !fs::is_directory(fs::symlink_status(location))) {
            throw std::runtime_error("package: expected directory without links");
        }
        const auto root = fs::canonical(location);
        std::ifstream file(package_file(root, "plugin.json", "manifest"), std::ios::binary);
        if (!file) {
            throw std::runtime_error("manifest: cannot open file");
        }
        std::string body(64 * 1024 + 1, '\0');
        file.read(body.data(), static_cast<std::streamsize>(body.size()));
        body.resize(static_cast<std::size_t>(file.gcount()));
        if (file.bad() || body.size() > 64 * 1024) {
            throw std::runtime_error("manifest: unreadable or exceeds 64 KiB");
        }

        std::set<std::string> keys;
        const auto object = Json::parse(body, [&](int depth, Json::parse_event_t event, Json& value) {
            if (depth == 1 && event == Json::parse_event_t::key && !keys.insert(value.get<std::string>()).second) {
                throw std::runtime_error("manifest: duplicate field");
            }
            return true;
        });
        const std::set<std::string> allowed{"manifest_version", "id", "name", "version", "abi_version", "entry", "default_config", "dependencies"};
        if (!object.is_object()) {
            throw std::runtime_error("manifest: expected object");
        }
        for (const auto& key : keys) {
            if (!allowed.contains(key)) {
                throw std::runtime_error("manifest: unknown field");
            }
        }
        if (!object.contains("manifest_version") || !object["manifest_version"].is_number_integer() ||
            object["manifest_version"] != 1) {
            throw std::runtime_error("manifest_version: expected integer 1");
        }
        if (!object.contains("abi_version") || !object["abi_version"].is_number_integer() ||
            object["abi_version"] < 1 || object["abi_version"] > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("abi_version: expected positive uint32");
        }

        Manifest manifest{root, text(object, "id", 64), text(object, "name", 256), text(object, "version", 64),
            object["abi_version"].get<std::uint32_t>(), {}, {}};
        if (!package_id(manifest.id)) {
            throw std::runtime_error("id: expected lowercase portable identifier");
        }
        manifest.dependencies = read_dependencies(object);
        const auto entry = text(object, "entry", 1024);
        if (!std::regex_search(entry, std::regex(R"(\.dll$)", std::regex::icase))) {
            throw std::runtime_error("entry: expected .dll filename");
        }
        manifest.entry = package_file(root, entry, "entry");
        if (object.contains("default_config")) {
            manifest.default_config = package_file(root, text(object, "default_config", 1024), "default_config");
        }
        return manifest;
    } catch (const Json::exception&) {
        throw std::runtime_error("manifest: invalid JSON");
    } catch (const fs::filesystem_error&) {
        throw std::runtime_error("package: filesystem error");
    }
}
}
