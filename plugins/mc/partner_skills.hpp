#pragma once
#include <array>
#include <cstdint>
#include <string_view>

namespace dfy::mc {
// 行为分类使用稳定键，回合控制工具常驻并单独标注。
inline constexpr std::array categories{"movement","dig_break","place","use_item","attack","block_interaction",
    "entity_interaction","inventory","craft_process","communication","observation","query"};
enum class SkillCategory : std::uint8_t {
    movement,dig_break,place,use_item,attack,block_interaction,entity_interaction,inventory,craft_process,
    communication,observation,query,control
};
enum class SkillKind { game, confirmed, task, catalog, question };
struct PartnerSkill {
    std::string_view name, purpose, description, schema;
    SkillCategory category;
    bool changes;
    SkillKind kind = SkillKind::game;
    bool supports_approval = false;
};
inline constexpr std::array<PartnerSkill,9> partner_skills{{
    {"observe", "partner.observe", "Read current positions, landmarks and inventory. Optional block registry name lists visible matches. For structures use around=player or self. survey contains client-loaded blocks from origin, with x increasing across each row, z across rows, y across layers. Palette decodes block symbols; dot is air, question mark is unloaded, asterisk is an unlisted block.",
        R"({"type":"object","properties":{"block":{"type":"string"},"around":{"type":"string","enum":["player","self"]}},"additionalProperties":false})", SkillCategory::observation, false},
    {"move", "partner.move", "Walk without digging to a named landmark, player (place=player), or one nearby integer x,y,z position.",
        R"({"type":"object","properties":{"place":{"type":"string"},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"additionalProperties":false})", SkillCategory::movement, true},
    {"collect", "partner.collect", "Gather visible blocks by Minecraft registry name and count (1-32). For a specific block supply x,y,z and count=1. When consent is needed, set approval to game or main. Reports broken blocks and actual inventory changes.",
        R"({"type":"object","properties":{"block":{"type":"string"},"count":{"type":"integer"},"approval":{"type":"string","enum":["game","main"]},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"required":["block","count"],"additionalProperties":false})", SkillCategory::dig_break, true, SkillKind::game, true},
    {"reply", "partner.reply", "Speak to the bound player in this game. Call only when you choose to speak. Internal turn completion is silent.",
        R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"],"additionalProperties":false})", SkillCategory::communication, true},
    {"place", "partner.place", "Place one inventory block at the exact empty x, y, z cell, or one door with its lower half at that cell. Survey the intended structure first. Uses a visible support and reads back the changed cells. Optional approval selects game or main.",
        R"({"type":"object","properties":{"block":{"type":"string"},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"},"approval":{"type":"string","enum":["game","main"]}},"required":["block","x","y","z"],"additionalProperties":false})", SkillCategory::place, true, SkillKind::game, true},
    {"collect_confirmed", "partner.collect_confirmed", "Request player consent for a fixed collection. Route 'game' asks in the game; route 'main' lets the main Agent select a channel. The original collection runs only after consent.",
        R"({"type":"object","properties":{"block":{"type":"string"},"count":{"type":"integer"},"route":{"type":"string","enum":["game","main"]},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"required":["block","count"],"additionalProperties":false})", SkillCategory::dig_break, true, SkillKind::confirmed},
    {"task_update", "partner.task_update", "Update the shared task within this world. Use the latest revision, retain known facts and uncertainties in detail. next: continue after this turn, scene on observed change, player on new input, done when complete.",
        R"({"type":"object","properties":{"revision":{"type":"integer"},"goal":{"type":"string"},"detail":{"type":"string"},"next":{"type":"string","enum":["continue","scene","player","done"]}},"required":["revision","goal","next"],"additionalProperties":false})", SkillCategory::control, true, SkillKind::task},
    {"ask_player", "partner.ask_player", "Record a clarification and ask the bound player. If no reply arrives locally, escalate using direct ASK by default or through the main Agent. Wait for questionResult before relying on the answer.",
        R"({"type":"object","properties":{"question":{"type":"string"},"path":{"type":"string","enum":["direct","main"]}},"required":["question"],"additionalProperties":false})", SkillCategory::control, true, SkillKind::question},
    {"skills", "partner.skills", "List Minecraft skill categories and availability. Request up to three category keys in categories to include their available tools with full parameters in the next decision.",
        R"({"type":"object","properties":{"categories":{"type":"array","items":{"type":"string"}}},"additionalProperties":false})", SkillCategory::query, true, SkillKind::catalog}
}};
}
