#include "persona.hpp"
#include <dafeiyu/digest.hpp>
#include <dafeiyu/utf8.hpp>
#include <fstream>
#include <nlohmann/json.hpp>

namespace dfy::app {
// 先装配本体人格，再保留调用入口提供的场景约束。
void PersonaProfile::apply(agent::RunOptions& options) const {
    options.version_labels.persona_profile = version;
    options.system_instructions = options.system_instructions.empty() ? instructions : instructions+"\n"+options.system_instructions;
}

// 读取受信任的本体人格档案，缺省人格独立于任何插件。
// 步骤：检查文件存在 → 在固定大小内读取 → 核对档案版本及正文 → 生成可比较的公开标识。
PersonaProfile load_persona(const std::filesystem::path& user_data) {
    const auto file = user_data/"persona.json";
    if (!std::filesystem::exists(file)) return {"dafeiyu/default/1",
        "你叫大肥鱼。和对方熟悉地说话，平时简短、自然，像在一起做事的伙伴。"
        "先接住对方真正说的话，只有对方关心的进展或必要问题值得打断。"
        "用你实际知道的事实作答，不虚构所见、经历与进度，也不朗读内部流程。"};

    std::ifstream stream(file,std::ios::binary);
    if (!stream) throw std::runtime_error("persona: cannot read persona.json");
    std::string bytes(4097,'\0');
    stream.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(stream.gcount()));
    if (stream.bad() || bytes.size() > 4096) throw std::invalid_argument("persona: invalid file size");
    const auto object = nlohmann::json::parse(bytes,nullptr,false);
    if (!object.is_object() || object.size() != 3 || !object.contains("id") || !object.at("id").is_string() ||
        !object.contains("revision") || !object.at("revision").is_number_unsigned() || !object.contains("instructions") ||
        !object.at("instructions").is_string()) throw std::invalid_argument("persona: expected id, revision and instructions");
    const auto& id = object.at("id").get_ref<const std::string&>();
    const auto revision = object.at("revision").get<std::uint32_t>();
    const auto& instructions = object.at("instructions").get_ref<const std::string&>();
    if (id.empty() || id.size() > 32 || id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-_") != id.npos ||
        !revision || object.at("revision") != revision || instructions.size() > 2048 ||
        instructions.find_first_not_of(" \r\n\t") == instructions.npos || !runtime::valid_utf8(instructions))
        throw std::invalid_argument("persona: invalid profile");
    return {id+"/"+std::to_string(revision)+"/"+platform::sha256(instructions),instructions};
}
}
