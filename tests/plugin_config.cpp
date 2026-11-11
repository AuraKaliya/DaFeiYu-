#include <dafeiyu/plugin_config.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using namespace dfy::plugins;

// 在配置行为不符合契约时报告测试失败。
void check(bool condition, const char* reason) {
    if (!condition) {
        throw std::runtime_error(reason);
    }
}

// 写入隔离测试文件并检查输出流状态。
void write(const fs::path& path, const std::string& body) {
    std::ofstream file(path, std::ios::binary);
    file << body;
    file.close();
    check(static_cast<bool>(file), "cannot write test file");
}

// 创建指定 ID 的最小目录包，并按需声明已有默认配置文件。
// 步骤：准备目录及文本入口 → 写入清单 → 读取正式包描述。
Manifest package(const fs::path& path, const std::string& id, bool defaults = false) {
    fs::create_directories(path);
    write(path / "entry.dll", "not a DLL");
    Json manifest{{"manifest_version", 1}, {"id", id}, {"name", "配置测试"},
        {"version", "1"}, {"abi_version", 1}, {"entry", "entry.dll"}};
    if (defaults) {
        manifest["default_config"] = "defaults.json";
    }
    write(path / "plugin.json", manifest.dump());
    return load_manifest(path);
}

// 检查错误类别并确认错误信息不携带配置内容。
// 步骤：执行预期失败的读取 → 核对错误类别与敏感标记隔离。
void rejects(const Manifest& manifest, const fs::path& users, const std::string& expected) {
    try {
        (void)load_config(manifest, users);
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        check(message.find(expected) != std::string::npos && message.find("private-test-marker") == std::string::npos,
            "unexpected or sensitive configuration error");
        return;
    }
    throw std::runtime_error("invalid plugin configuration accepted");
}
}

// 验证独立配置合并和快照存活，并覆盖非法内容与路径边界。
// 步骤：检查空配置与单侧配置 → 核对顶层覆盖及插件隔离 → 检查非法内容 → 拒绝缺失文件和链接。
int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected work directory");
        fs::create_directories(argv[1]);
        fs::path run;
        for (unsigned i = 1; ; ++i) {
            run = fs::path(argv[1]) / std::to_string(i);
            if (fs::create_directory(run)) {
                break;
            }
        }
        const auto root = run / u8"中文 包";
        const auto users = run / u8"用户 数据";
        auto manifest = package(root, "sample.one");
        const auto empty = load_config(manifest, users);
        check(empty->plugin_id == manifest.id && empty->json == "{}" && !fs::exists(users), "empty defaults created files");
        const auto user_dir = users / "plugins/sample.one";
        fs::create_directories(user_dir);
        const auto settings = user_dir / "settings.json";
        const auto user_only = Json::parse(R"({"user_only":true,"items":[{"key":1},{"key":2}]})");
        auto user_body = "\xEF\xBB\xBF" + user_only.dump();
        user_body.resize(64 * 1024, ' ');
        write(settings, user_body);
        check(Json::parse(load_config(manifest, users)->json) == user_only, "user-only config or size boundary failed");

        const Json defaults{{"keep", 1}, {"over", 2}, {"nested", {{"left", 1}, {"right", 2}}},
            {"list", {1, 2}}, {"nullable", 3}};
        write(root / "defaults.json", "\xEF\xBB\xBF" + defaults.dump());
        manifest = package(root, "sample.one", true);
        fs::remove(settings);
        check(Json::parse(load_config(manifest, users)->json) == defaults, "defaults-only config changed");
        const Json overrides{{"over", 7}, {"nested", {{"right", 9}}}, {"list", {3}}, {"nullable", nullptr}, {"added", "中文"}};
        write(settings, overrides.dump());
        const auto snapshot = load_config(manifest, users);
        const Json expected{{"keep", 1}, {"over", 7}, {"nested", {{"right", 9}}},
            {"list", {3}}, {"nullable", nullptr}, {"added", "中文"}};
        check(Json::parse(snapshot->json) == expected && empty->json == "{}", "top-level merge or retained snapshot changed");
        write(settings, "{}");
        check(Json::parse(load_config(manifest, users)->json) == defaults && Json::parse(snapshot->json) == expected,
            "reload changed old snapshot");
        const auto second = package(run / "second", "sample.two");
        fs::create_directory(users / "plugins/sample.two");
        write(users / "plugins/sample.two/settings.json", R"({"owner":"two"})");
        check(Json::parse(load_config(second, users)->json) == Json{{"owner", "two"}} &&
                  load_config(second, users)->plugin_id == "sample.two", "plugin settings mixed");

        std::string nested = "0";
        for (int depth = 0; depth < 64; ++depth) {
            nested = "{\"key\":" + nested + "}";
        }
        write(settings, nested);
        (void)load_config(manifest, users);
        write(settings, "{}");
        for (const auto& path : {root / "defaults.json", settings}) {
            for (const auto& invalid : {std::string("[]"), std::string("null"), std::string("{private-test-marker}"),
                     std::string(R"({"key":1,"key":2})"), std::string(R"({"nested":[{"key":1,"key":2}]})"),
                     std::string(64 * 1024 + 1, ' '), std::string("{\"key\":\"") + static_cast<char>(0xFF) + "\"}",
                     "{\"key\":" + nested + "}"}) {
                write(path, invalid);
                rejects(manifest, users, "plugin config:");
            }
            write(path, "{}");
        }
        auto forged = manifest;
        forged.id = "../escape";
        rejects(forged, users, "manifest changed");
        fs::remove(settings);
        fs::create_directory(settings);
        rejects(manifest, users, "invalid user path");
        fs::remove(settings);
        write(run / "outside.json", "{}");
        fs::create_symlink(fs::absolute(run / "outside.json"), settings);
        rejects(manifest, users, "invalid user path");
        fs::remove(settings);
        write(settings, "{}");
        fs::create_directory_symlink(fs::absolute(users), run / "linked-users");
        rejects(manifest, run / "linked-users", "invalid user path");
        fs::create_directory(run / "other-users");
        fs::create_directory_symlink(fs::absolute(users / "plugins"), run / "other-users/plugins");
        rejects(manifest, run / "other-users", "invalid user path");
        fs::create_directories(run / "linked-id/plugins");
        fs::create_directory_symlink(fs::absolute(user_dir), run / "linked-id/plugins/sample.one");
        rejects(manifest, run / "linked-id", "invalid user path");
        fs::remove(root / "defaults.json");
        rejects(manifest, users, "default_config:");
        fs::create_symlink(fs::absolute(run / "outside.json"), root / "defaults.json");
        rejects(manifest, users, "default_config:");
        check(Json::parse(snapshot->json) == expected, "failed reload damaged prior snapshot");
        std::cout << "Plugin configuration checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
