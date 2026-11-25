#pragma once
#include <dafeiyu/plugin_manifest.hpp>
#include <memory>

namespace dfy::plugins {
struct EffectiveConfig {
    std::string plugin_id;
    std::string json;
};

// 复核清单后合并默认值与用户数据根目录下的 plugins/<id>/settings.json，返回只读 UTF-8 JSON 对象快照。
std::shared_ptr<const EffectiveConfig> load_config(const Manifest& manifest, const std::filesystem::path& user_data_root);
}
