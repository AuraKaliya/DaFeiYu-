#include <dafeiyu/plugin_config.hpp>
#include "package_files.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <regex>
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
    if (files::is_reparse_point(path) || !fs::is_regular_file(fs::symlink_status(path))) {
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

// 只从原包 agent/status.json 读取注册表，实例内沿用加载时的原件。
// 步骤：检查固定目录和文件 → 有界读取注册表 → 核对包拥有的键并保存定义。
void load_status(const Manifest& manifest, EffectiveConfig& config) {
    const auto agent = manifest.directory / "agent";
    if (files::is_reparse_point(agent)) throw std::runtime_error("plugin config: linked agent directory");
    if (!fs::exists(fs::symlink_status(agent))) return;
    const auto path = files::directory(agent) / "status.json";
    if (!fs::exists(fs::symlink_status(path))) return;
    const auto doc = read_object(path,"agent status definitions");
    if (doc.size()!=(doc.contains("readWith") ? 3 : 2) || doc.value("schema",std::string{})!="dafeiyu.status_definitions/1" ||
        !doc.contains("definitions") || !doc.at("definitions").is_array() || doc.at("definitions").size()>32)
        throw std::runtime_error("plugin config: invalid agent status registration");
    const auto& definitions = doc.at("definitions");
    if (!definitions.empty() && (manifest.id=="host" || manifest.id.starts_with("host.")))
        throw std::runtime_error("plugin config: reserved agent status owner");
    for (const auto& field : definitions)
        if (!field.is_object() || !field.contains("key") || !field.at("key").is_string() ||
            !field.at("key").get_ref<const std::string&>().starts_with(manifest.id+"."))
            throw std::runtime_error("plugin config: foreign agent status owner");
    if (doc.contains("readWith")) {
        const auto read = doc.at("readWith").get<std::string>();
        const auto prefix = manifest.id+"/";
        if (definitions.empty() || !read.starts_with(prefix) || read.size()==prefix.size() || read.size()>128 ||
            read.find('/',prefix.size())!=read.npos) throw std::runtime_error("plugin config: invalid status reader grant");
        config.status_read_with = read;
    }
    config.status_definitions = definitions.dump();
}

// 从与包身份匹配的可选 UI 描述登记只读文本，只投影已合并到本代次的同名顶层字段。
// 步骤：定位包内普通描述文件 → 核对版本与身份 → 逐项复核声明和值 → 整批发布或隔离登记错误。
PublicSettings public_settings(const Manifest& manifest, const Json& effective, const Json& overrides) {
    try {
        const auto ui=manifest.directory/"ui";
        if (files::is_reparse_point(ui)) throw std::runtime_error("settings: linked UI directory");
        if (!fs::exists(fs::symlink_status(ui))) return {};
        const auto dir=files::directory(ui);
        if (dir.parent_path()!=manifest.directory) throw std::runtime_error("settings: invalid UI directory");
        const auto path=dir/"manifest.json";
        if (files::is_reparse_point(path)) throw std::runtime_error("settings: linked UI manifest");
        if (!fs::exists(fs::symlink_status(path))) return {};
        const auto ui_manifest=read_object(path,"UI registration");
        const auto& version=ui_manifest.at("ui_schema_version");
        const auto& contributions=ui_manifest.at("contributions");
        if (!version.is_number_integer() || version!=1 || ui_manifest.at("plugin_id")!=manifest.id ||
            !contributions.is_array() || contributions.size()>64) throw std::runtime_error("settings: invalid UI registration");
        PublicSettings result;
        std::set<std::string> contributions_seen, seen;
        static const std::regex field_id("[A-Za-z][A-Za-z0-9_.-]{0,63}");
        for (const auto& contribution : contributions) {
            if (contribution.at("kind")!="settings") continue;
            if (ui_manifest.contains("requires") && (!ui_manifest["requires"].is_array() || !ui_manifest["requires"].empty()))
                throw std::runtime_error("settings: unsupported UI requirement");
            const auto& fields=contribution.at("fields");
            const auto& identity=contribution.at("id").get_ref<const std::string&>();
            if (identity.size()>64 || !std::regex_match(identity,field_id) || !contributions_seen.insert(identity).second ||
                contribution.size()!=3u+contribution.contains("title") || (contribution.contains("title") && !contribution.at("title").is_string()) ||
                !fields.is_array() || fields.empty() || fields.size()>32-result.fields.size())
                throw std::runtime_error("settings: invalid field registration");
            for (const auto& field : fields) {
                const auto& id=field.at("id").get_ref<const std::string&>();
                if (field.size()!=3 || field.at("type")!="string" || field.at("access_mode")!="read_only" ||
                    id.size()>64 || !std::regex_match(id,field_id) || !seen.insert(id).second)
                    throw std::runtime_error("settings: invalid public field");
                const auto& value=effective.at(id).get_ref<const std::string&>();
                if (value.size()>4096) throw std::runtime_error("settings: public text too long");
                result.fields.push_back({id,value,overrides.contains(id)});
            }
            result.state=PublicSettingsState::ready;
        }
        return result;
    } catch (const std::exception&) { return {PublicSettingsState::unavailable,{}}; }
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
        if (files::is_reparse_point(path)) throw std::runtime_error("plugin config: invalid user path or link");
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
        EffectiveConfig config{current.id,effective.dump(),public_settings(current,effective,overrides)};
        load_status(current,config);
        return std::make_shared<const EffectiveConfig>(std::move(config));
    } catch (const fs::filesystem_error&) {
        throw std::runtime_error("plugin config: filesystem error");
    }
}
}
