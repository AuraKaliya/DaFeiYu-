#include "common.hpp"
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace dfy {
int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::string identifier() {
    std::random_device source;
    std::ostringstream out;
    for (int i = 0; i < 4; ++i) out << std::hex << std::setw(8) << std::setfill('0') << source();
    return out.str();
}
std::string fingerprint(std::string_view text) {
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char byte : text) { hash ^= byte; hash *= 1099511628211ull; }
    std::ostringstream out; out << std::hex << hash; return out.str();
}
void require(bool condition, const std::string& code) { if (!condition) throw std::runtime_error(code); }
std::string read_text(const Path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.good(), "FILE_OPEN");
    const auto length = std::filesystem::file_size(path);
    require(length <= 4 * 1024 * 1024, "FILE_SIZE_LIMIT");
    std::string value(static_cast<size_t>(length), '\0');
    require(static_cast<bool>(stream.read(value.data(), static_cast<std::streamsize>(length))), "FILE_READ");
    return value;
}
Json read_json(const Path& path) { return Json::parse(read_text(path)); }
bool includes(const Json& values, const Json& value) { return std::find(values.begin(), values.end(), value) != values.end(); }
bool terminal(const Json& run) { return includes(Json::array({"completed", "failed", "cancelled"}), run.at("execution")); }
Json failure(const std::exception& error) {
    const std::string code = error.what();
    const bool denied = code.find("DENIED") != std::string::npos;
    return sdk::error(code.substr(0, 2048), denied ? "permission" : "protocol");
}
void validate_outcome(const Json& value) {
    require(value.is_object() && includes(Json::array({"succeeded", "partial", "failed", "cancelled", "unknown"}), value.at("status")), "OUTCOME_STATUS");
    require(includes(Json::array({"none", "confirmed", "unknown"}), value.at("effects")), "OUTCOME_EFFECTS");
    require(includes(Json::array({"complete", "partial", "unknown"}), value.at("coverage")), "OUTCOME_COVERAGE");
    require(value.contains("data") && value.at("sources").is_array() && value.at("receipts").is_array(), "OUTCOME_INVALID");
}
Profile Profile::open(const Path& file) {
    const auto path = std::filesystem::absolute(file);
    auto data = read_json(path);
    require(data.at("schema_version") == 2, "PROFILE_VERSION_UNSUPPORTED");
    require(!data.at("owner").get<std::string>().empty(), "PROFILE_OWNER");
    require(data.at("bindings").is_object() && data.at("plugins").is_array(), "PROFILE_SERVICES");
    for (const auto* name : {"model", "memory", "channel", "strategy"}) require(data.at("bindings").contains(name), "PROFILE_BINDING_MISSING");
    require(data.at("allowed_actions").is_array() && data.at("allowed_effects").is_array(), "PROFILE_GRANTS");
    for (const auto* name : {"model_attempts", "strategy_steps", "call_timeout_ms", "max_jobs", "max_inbox", "shutdown_ms"})
        require(data.at("limits").at(name).is_number_integer() && data.at("limits").at(name).get<int64_t>() > 0, "PROFILE_LIMIT");
    require(data.at("limits").at("max_jobs").get<int>() <= 256, "PROFILE_JOB_LIMIT");
    for (const auto& plugin : data.at("plugins")) require(!plugin.at("revision").get<std::string>().empty(), "CONFIG_REVISION_REQUIRED");
    return {data, path.parent_path() / sdk::path_from_utf8(data.at("plugin_directory").get<std::string>()),
        read_text(path.parent_path() / sdk::path_from_utf8(data.at("foundation").get<std::string>()))};
}
}
