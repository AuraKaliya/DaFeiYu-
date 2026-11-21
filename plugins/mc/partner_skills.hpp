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
enum class SkillKind { game, confirmed, task, catalog, question, motion, motion_stop, approval_withdraw };
struct PartnerSkill {
    std::string_view name, purpose, description, schema;
    SkillCategory category;
    bool changes;
    SkillKind kind = SkillKind::game;
    bool supports_approval = false;
};
inline constexpr std::array<PartnerSkill,13> partner_skills{{
    {"observe", "partner.observe", "Read current positions and inventory. Use message=<original sequence> to re-read its world and playerFocus for 'this gap'. player is live, selfLook is your own view. blockSearch states the search scope; visible matches require your line of sight. doors groups verified halves by lower cell; incomplete doors are unconfirmed. survey uses x across rows, z across lines and y across layers. Dot is air, '?' is unloaded.",
        R"({"type":"object","properties":{"block":{"type":"string"},"around":{"type":"string","enum":["player","self"]},"message":{"type":"integer"}},"additionalProperties":false})", SkillCategory::observation, false},
    {"move", "partner.move", "Walk without digging to a landmark, player (place=player), or integer x,y,z. An accepted result is a running work ID. Read actual arrival in the next activity context.",
        R"({"type":"object","properties":{"place":{"type":"string"},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"additionalProperties":false})", SkillCategory::movement, true},
    {"start_move", "partner.start_move", "Start moving toward one target: original message position (message), a landmark or current player (place), or xyz. Returns at acceptance when in flight. Movement continues while you listen and reply.",
        R"({"type":"object","properties":{"place":{"type":"string"},"message":{"type":"integer"},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"additionalProperties":false})", SkillCategory::movement, true, SkillKind::motion},
    {"follow", "partner.follow", "Keep following the bound player as they move until stopped, lost or blocked. Returns at acceptance. Optional distance is 2-5 blocks, default 3.",
        R"({"type":"object","properties":{"distance":{"type":"integer"}},"additionalProperties":false})", SkillCategory::movement, true, SkillKind::motion},
    {"stop_motion", "partner.stop_motion", "Request cancellation of the matching work ID from activeMotion. Wait for its exit before starting another physical action.",
        R"({"type":"object","properties":{"work":{"type":"string"}},"required":["work"],"additionalProperties":false})", SkillCategory::control, true, SkillKind::motion_stop},
    {"collect", "partner.collect", "Gather visible blocks by Minecraft registry name and count (1-32). For a specific block supply x,y,z and count=1. Set approval to game or main when consent is needed. For accepted work, read actual broken blocks and inventory changes on exit.",
        R"({"type":"object","properties":{"block":{"type":"string"},"count":{"type":"integer"},"approval":{"type":"string","enum":["game","main"]},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"required":["block","count"],"additionalProperties":false})", SkillCategory::dig_break, true, SkillKind::game, true},
    {"reply", "partner.reply", "Send a one-line private message to the bound player now. Once called, final text is internal and is not sent again.",
        R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"],"additionalProperties":false})", SkillCategory::communication, true},
    {"place", "partner.place", "Place one inventory block at the exact empty x,y,z cell, or a door with its lower half there. Survey first. Set approval to game or main when needed. For accepted work, read actual changed cells on exit.",
        R"({"type":"object","properties":{"block":{"type":"string"},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"},"approval":{"type":"string","enum":["game","main"]}},"required":["block","x","y","z"],"additionalProperties":false})", SkillCategory::place, true, SkillKind::game, true},
    {"collect_confirmed", "partner.collect_confirmed", "Request player consent for a fixed collection. Route 'game' asks in the game; route 'main' lets the main Agent select a channel. The original collection runs only after consent.",
        R"({"type":"object","properties":{"block":{"type":"string"},"count":{"type":"integer"},"route":{"type":"string","enum":["game","main"]},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"required":["block","count"],"additionalProperties":false})", SkillCategory::dig_break, true, SkillKind::confirmed},
    {"task_update", "partner.task_update", "Update the shared task. The plugin assigns the revision. Give target.message and either a candidate ID from playerFocus or exact cells for an explicit coordinate instruction. Omit target to retain it, or use {} to clear it. next: continue, scene, player or done.",
        R"({"type":"object","properties":{"goal":{"type":"string"},"detail":{"type":"string"},"next":{"type":"string","enum":["continue","scene","player","done"]},"target":{"type":"object","properties":{"message":{"type":"integer"},"candidate":{"type":"string"},"cells":{"type":"array","items":{"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"required":["x","y","z"],"additionalProperties":false}}},"additionalProperties":false}},"required":["goal","next"],"additionalProperties":false})", SkillCategory::control, true, SkillKind::task},
    {"ask_player", "partner.ask_player", "Record one clarification. Choose local game presentation, direct ASK (default), or main Agent routing. Continue handling other messages. Read questionResult before using the answer.",
        R"({"type":"object","properties":{"question":{"type":"string"},"path":{"type":"string","enum":["local","direct","main"]}},"required":["question"],"additionalProperties":false})", SkillCategory::control, true, SkillKind::question},
    {"withdraw_confirmation", "partner.withdraw_confirmation", "Withdraw the waiting item from pendingApproval when the candidate is obsolete. Verify any new inventory or world state before deciding what to do next.",
        R"({"type":"object","properties":{"item":{"type":"integer"}},"required":["item"],"additionalProperties":false})", SkillCategory::control, true, SkillKind::approval_withdraw},
    {"skills", "partner.skills", "List Minecraft skill categories and availability. Request up to three category keys in categories to include their available tools with full parameters in the next decision.",
        R"({"type":"object","properties":{"categories":{"type":"array","items":{"type":"string"}}},"additionalProperties":false})", SkillCategory::query, true, SkillKind::catalog}
}};
}
