#pragma once
#include <dafeiyu/plugin_manifest.hpp>
#include <memory>
#include <string>
#include <vector>

namespace dfy::plugins {
struct PublicSetting {
    std::string id, value;
    bool user_override = false;
};
enum class PublicSettingsState { not_declared, ready, unavailable };
struct PublicSettings {
    PublicSettingsState state = PublicSettingsState::not_declared;
    std::vector<PublicSetting> fields;
};
struct EffectiveConfig {
    std::string plugin_id;
    std::string json;
    PublicSettings public_settings;
};

// 复核清单后合并默认值与用户数据根目录下的 plugins/<id>/settings.json，返回只读 UTF-8 JSON 对象快照。
std::shared_ptr<const EffectiveConfig> load_config(const Manifest& manifest, const std::filesystem::path& user_data_root);
}
