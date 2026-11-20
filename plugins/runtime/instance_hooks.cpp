#include <dafeiyu/plugin_instance.hpp>
#include <dafeiyu/utf8.hpp>

namespace dfy::plugins {
namespace {
constexpr uint32_t maximum_registrations = 32;
constexpr uint32_t scopes = DFY_V2_HOOK_MAIN_SESSION | DFY_V2_HOOK_OWN_SESSION | DFY_V2_HOOK_OTHER_SESSION;
}
// 读取插件启动时确定的 Hook 注册，整个列表通过校验才交给调用方。
// 步骤：协商可选能力 → 有界枚举 → 校验作用域与控制点 → 保存原序号顺序。
std::vector<dfy_v2_hook_description> PluginInstance::load_hooks() {
    const auto capability = query<dfy_v2_hook_api>(DFY_V2_HOOK_INTERFACE,DFY_V2_HOOK_VERSION);
    if (!capability) return {};
    return invoke(*capability,[](const auto& api, auto* instance) {
        if (!api.describe || !api.decide) throw std::logic_error("hooks: incomplete capability");
        std::vector<dfy_v2_hook_description> registrations;
        for (uint32_t index = 0; index <= maximum_registrations; ++index) {
            dfy_v2_hook_description registration{};
            const auto result = api.describe(instance,index,&registration,nullptr);
            if (result == DFY_V2_NOT_FOUND && !registration.point && !registration.sessions) return registrations;
            if (result || registration.point < DFY_V2_HOOK_MODEL_REQUEST || registration.point > DFY_V2_HOOK_TOOL_RESULT ||
                !registration.sessions || (registration.sessions & ~scopes)) throw std::invalid_argument("hooks: invalid registration");
            if (index == maximum_registrations) throw std::length_error("hooks: registration capacity exceeded");
            registrations.push_back(registration);
        }
        throw std::logic_error("hooks: registration enumeration did not end");
    });
}

// 对注册的原序号调用控制接口，未知输出和回调失败由宿主拒绝。
// 步骤：核对索引与阶段 → 进入原模块的活动保护 → 验证决定。
uint32_t PluginInstance::decide_hook(std::size_t index, const dfy_v2_hook_input& input) {
    const auto registrations = hooks();
    if (index >= registrations.size() || registrations[index].point != input.point)
        throw std::invalid_argument("hooks: registration and point mismatch");
    const auto capability = query<dfy_v2_hook_api>(DFY_V2_HOOK_INTERFACE,DFY_V2_HOOK_VERSION);
    if (!capability) throw std::logic_error("hooks: registered interface is unavailable");
    return invoke(*capability,[&](const auto& api, auto* instance) {
        uint32_t verdict = 0;
        if (!api.decide || api.decide(instance,static_cast<uint32_t>(index),&input,&verdict,nullptr) ||
            (verdict != DFY_V2_HOOK_ALLOW && verdict != DFY_V2_HOOK_DENY &&
                (verdict != DFY_V2_HOOK_CONFIRM || input.point != DFY_V2_HOOK_TOOL_START)))
            throw std::runtime_error("hooks: invalid control decision");
        return verdict;
    });
}

// 读取有限数量的文本 Hook 并验证控制点、作用域与接续方式。
// 步骤：协商可选能力 → 校验完整枚举 → 按原序号保存注册。
std::vector<dfy_v2_text_hook_description> PluginInstance::load_text_hooks() {
    const auto capability = query<dfy_v2_text_hook_api>(DFY_V2_TEXT_HOOK_INTERFACE,DFY_V2_TEXT_HOOK_VERSION);
    if (!capability) return {};
    return invoke(*capability,[](const auto& api, auto* instance) {
        if (!api.describe || !api.rewrite) throw std::logic_error("text hooks: incomplete capability");
        std::vector<dfy_v2_text_hook_description> registrations;
        for (uint32_t index = 0; index <= maximum_registrations; ++index) {
            dfy_v2_text_hook_description registration{};
            const auto result = api.describe(instance,index,&registration,nullptr);
            if (result == DFY_V2_NOT_FOUND && !registration.point && !registration.sessions && !registration.flags) return registrations;
            if (result || registration.point < DFY_V2_TEXT_HOOK_INPUT || registration.point > DFY_V2_TEXT_HOOK_REPLY ||
                !registration.sessions || (registration.sessions & ~scopes) || (registration.flags & ~DFY_V2_TEXT_HOOK_CHAIN))
                throw std::invalid_argument("text hooks: invalid registration");
            if (index == maximum_registrations) throw std::length_error("text hooks: registration capacity exceeded");
            registrations.push_back(registration);
        }
        throw std::logic_error("text hooks: registration enumeration did not end");
    });
}

// 在插件活动保护期内接收和验证有界的宿主缓冲结果。
// 步骤：核对原注册 → 调用原 DLL → 接收保持原值指示或验证 UTF-8 后复制。
std::optional<std::string> PluginInstance::rewrite_hook(std::size_t index, const dfy_v2_text_hook_input& input, std::span<char> output) {
    const auto registrations = text_hooks();
    if (index >= registrations.size() || registrations[index].point != input.point)
        throw std::invalid_argument("text hooks: registration and point mismatch");
    const auto capability = query<dfy_v2_text_hook_api>(DFY_V2_TEXT_HOOK_INTERFACE,DFY_V2_TEXT_HOOK_VERSION);
    if (!capability) throw std::logic_error("text hooks: registered interface is unavailable");
    return invoke(*capability,[&](const auto& api, auto* instance) -> std::optional<std::string> {
        uint64_t count = DFY_V2_TEXT_HOOK_UNCHANGED;
        if (!api.rewrite || api.rewrite(instance,static_cast<uint32_t>(index),&input,output.data(),output.size(),&count,nullptr))
            throw std::runtime_error("text hooks: callback failed");
        if (count == DFY_V2_TEXT_HOOK_UNCHANGED) return std::nullopt;
        if (count > output.size()) throw std::length_error("text hooks: output byte limit exceeded");
        const std::string_view view(output.data(),static_cast<std::size_t>(count));
        if (!runtime::valid_utf8(view)) throw std::invalid_argument("text hooks: output must be valid UTF-8");
        return std::string(view);
    });
}
}
