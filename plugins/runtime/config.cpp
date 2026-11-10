#include <dafeiyu/plugin_config.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <set>
#include <stdexcept>
#include <vector>

namespace dfy::plugins {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;

// 读取有大小及嵌套限制的 JSON 对象，拒绝任何层级的重复字段且不回显配置内容。
// 步骤：有界读取文件 → 解析并检查对象键 → 确认根对象类型。
Json read_object(const fs::path& path, const char* source) {
    const auto prefix = std::string("plugin config: ") + source;
    if (!fs::is_regular_file(fs::symlink_status(path))) {
        throw std::runtime_error(prefix + ": expected regular file without links");
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error(prefix + ": cannot open file");
    }
    std::string body(64 * 1024 + 1, '\0');
    file.read(body.data(), static_cast<std::streamsize>(body.size()));
    body.resize(static_cast<std::size_t>(file.gcount()));
    if (file.bad() || body.size() > 64 * 1024) {
        throw std::runtime_error(prefix + ": unreadable or exceeds 64 KiB");
    }

    std::vector<std::set<std::string>> keys;
    Json object;
    try {
        // 跟踪解析中的对象键，拒绝过深的容器和同一对象内的重复字段。
        // 步骤：检查容器深度 → 按对象开始、字段或结束事件维护键集合。
        object = Json::parse(body, [&](int depth, Json::parse_event_t event, Json& value) {
            // 限制容器深度，避免后续序列化深层配置时耗尽调用栈。
            if ((event == Json::parse_event_t::object_start || event == Json::parse_event_t::array_start) && depth >= 64) {
                throw std::runtime_error(prefix + ": nesting exceeds 64 levels");
            }
            if (event == Json::parse_event_t::object_start) {
                keys.emplace_back();
            } else if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second) {
                throw std::runtime_error(prefix + ": duplicate field");
            } else if (event == Json::parse_event_t::object_end) {
                keys.pop_back();
            }
            return true;
        });
    } catch (const Json::exception&) {
        throw std::runtime_error(prefix + ": invalid JSON");
    }
    if (!object.is_object()) {
        throw std::runtime_error(prefix + ": expected object");
    }
    return object;
}

// 定位按插件 ID 隔离的用户配置，缺失路径按未配置处理，已有链接或错误类型明确拒绝。
// 步骤：检查用户根目录 → 逐段检查 plugins 与插件目录 → 确认配置文件及规范化位置。
std::optional<fs::path> user_file(const fs::path& user_root, const std::string& id) {
    auto path = fs::absolute(user_root).lexically_normal();
    if (path.has_relative_path() && path.filename().empty()) {
        path = path.parent_path();
    }
    fs::path parent;
    const fs::path parts[]{"plugins", id, "settings.json"};
    for (std::size_t i = 0; i < 4; ++i) {
        std::error_code error;
        const auto status = fs::symlink_status(path, error);
        if (error && error != std::errc::no_such_file_or_directory) {
            throw std::runtime_error("plugin config: cannot inspect user path");
        }
        if (!fs::exists(status)) {
            return std::nullopt;
        }
        if (i == 3 ? !fs::is_regular_file(status) : !fs::is_directory(status)) {
            throw std::runtime_error("plugin config: invalid user path or link");
        }
        const auto resolved = fs::canonical(path);
        if (i != 0 && resolved.parent_path() != parent) {
            throw std::runtime_error("plugin config: user path outside expected directory");
        }
        if (i == 3) {
            return resolved;
        }
        parent = resolved;
        path = parent / parts[i];
    }
    return std::nullopt;
}
}

// 生成独立配置快照，不写入用户目录或改变已返回的配置。
// 步骤：复核描述及文件边界 → 读取两侧配置 → 按顶层键覆盖 → 序列化为只读快照。
std::shared_ptr<const EffectiveConfig> load_config(const Manifest& manifest, const fs::path& user_data_root) {
    if (user_data_root.empty()) {
        throw std::invalid_argument("plugin config: user root required");
    }
    try {
        // 不直接信任可修改或过期的描述，避免使用已变更 ID 或越界文件定位配置。
        const auto current = load_manifest(manifest.directory);
        if (current != manifest) {
            throw std::runtime_error("plugin config: manifest changed; refresh catalog");
        }
        auto effective = current.default_config ? read_object(*current.default_config, "defaults") : Json::object();
        const auto user = user_file(user_data_root, current.id);
        const auto overrides = user ? read_object(*user, "user") : Json::object();

        for (const auto& [key, value] : overrides.items()) {
            effective[key] = value;
        }
        return std::make_shared<const EffectiveConfig>(EffectiveConfig{current.id, effective.dump()});
    } catch (const fs::filesystem_error&) {
        throw std::runtime_error("plugin config: filesystem error");
    }
}
}
