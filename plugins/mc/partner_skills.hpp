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
enum class SkillKind { game, confirmed, task, catalog, knowledge, question, motion, motion_stop, approval_withdraw, action_queue };
struct PartnerSkill {
    std::string_view name, purpose, description, schema;
    SkillCategory category;
    bool changes;
    SkillKind kind = SkillKind::game;
    bool supports_approval = false;
};
inline constexpr std::array<PartnerSkill,18> partner_skills{{
    {"observe", "partner.observe", "Read positions and inventory. For multiple views set sources to self, player, or message (with the original sequence). Set radius 1-5, detail summary or cells, and samples with count 1-3 and intervalMs 100-1000. Do not combine multiple views with around or block. Verify named xyz cells within the views with verify. Grid indexes run x, z, then y; '=' refers to an earlier view, '.' is air, '?' is unloaded, '*' exceeds palette. Delta compares known cells in both samples. Legacy block search reports visible matches and groups door halves.",
        R"({"type":"object","properties":{"block":{"type":"string"},"around":{"type":"string","enum":["player","self"]},"message":{"type":"integer"},"sources":{"type":"array","items":{"type":"string","enum":["self","player","message"]}},"radius":{"type":"integer"},"detail":{"type":"string","enum":["summary","cells"]},"verify":{"type":"array","items":{"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"required":["x","y","z"],"additionalProperties":false}},"samples":{"type":"object","properties":{"count":{"type":"integer"},"intervalMs":{"type":"integer"}},"additionalProperties":false}},"additionalProperties":false})", SkillCategory::observation, false},
    {"move", "partner.move", "Walk without digging to a landmark, player (place=player), or integer x,y,z. An accepted result is a running work ID. Read actual arrival in the next activity context.",
        R"({"type":"object","properties":{"place":{"type":"string"},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"additionalProperties":false})", SkillCategory::movement, true},
    {"start_move", "partner.start_move", "Start moving toward one target: original message position (message), a landmark or current player (place), or xyz. Returns at acceptance when in flight. Movement continues while you listen and reply.",
        R"({"type":"object","properties":{"place":{"type":"string"},"message":{"type":"integer"},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"additionalProperties":false})", SkillCategory::movement, true, SkillKind::motion},
    {"follow", "partner.follow", "Keep following the bound player as they move until stopped, lost or blocked. Returns at acceptance. Optional distance is 2-5 blocks, default 3.",
        R"({"type":"object","properties":{"distance":{"type":"integer"}},"additionalProperties":false})", SkillCategory::movement, true, SkillKind::motion},
    {"stop_motion", "partner.stop_motion", "Cancel following by the stable following.work ID, or cancel the matching activeMotion work. Actual exit is reported separately.",
        R"({"type":"object","properties":{"work":{"type":"string"}},"required":["work"],"additionalProperties":false})", SkillCategory::control, true, SkillKind::motion_stop},
    {"queue_actions", "partner.queue_actions", "Append up to four authorized steps. Interrupt pauses following for these one-shot steps and resumes it afterward. Collect requires one exact block. Set pickup to the inventory item required by later steps. Place needs a bound target. Clear discards waiting steps. Replace cancels the named work. A failed prerequisite discards successors in its group.",
        R"({"type":"object","properties":{"operation":{"type":"string","enum":["append","clear","replace","interrupt"]},"work":{"type":"string"},"steps":{"type":"array","items":{"type":"object","properties":{"skill":{"type":"string","enum":["move","follow","collect","place"]},"pickup":{"type":"string"},"arguments":{"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"},"message":{"type":"integer"},"place":{"type":"string"},"distance":{"type":"integer"},"block":{"type":"string"},"count":{"type":"integer"}},"additionalProperties":false}},"required":["skill","arguments"],"additionalProperties":false}}},"required":["operation"],"additionalProperties":false})", SkillCategory::control, true, SkillKind::action_queue},
    {"collect", "partner.collect", "Gather visible blocks by Minecraft registry name and count (1-32), within the task's collection scope. For a specific block supply x,y,z and count=1. Set approval to game or main when consent is needed. On exit, read actual cell changes separately from inventory changes.",
        R"({"type":"object","properties":{"block":{"type":"string"},"count":{"type":"integer"},"approval":{"type":"string","enum":["game","main"]},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"required":["block","count"],"additionalProperties":false})", SkillCategory::dig_break, true, SkillKind::game, true},
    {"dig_block", "partner.dig_block", "Dig the exact observed block at x,y,z. Give its expected Minecraft block name. The program approaches it, rechecks it before digging, and pauses and resumes following if needed. Read the eventual changed cells and actual pickup.",
        R"({"type":"object","properties":{"block":{"type":"string"},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"},"approval":{"type":"string","enum":["game","main"]}},"required":["block","x","y","z"],"additionalProperties":false})", SkillCategory::dig_break, true, SkillKind::motion, true},
    {"gather_ground", "partner.gather_ground", "Get enough of item for requiredStock using exposed natural terrain of the given block type. For four dirt blocks set requiredStock to 4, even when some dirt is already in the bag. Stops on enough stock or unverified pickup. Use knowledge for drop rules and read the actual inventory result before building.",
        R"({"type":"object","properties":{"block":{"type":"string"},"item":{"type":"string"},"requiredStock":{"type":"integer"},"approval":{"type":"string","enum":["game","main"]}},"required":["block","item","requiredStock"],"additionalProperties":false})", SkillCategory::dig_break, true, SkillKind::motion, true},
    {"reply", "partner.reply", "Send a one-line private message to the bound player now. Once called, final text is internal and is not sent again.",
        R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"],"additionalProperties":false})", SkillCategory::communication, true},
    {"place", "partner.place", "Place one inventory block at the exact empty x,y,z cell, or a door with its lower half there. Survey first. Set approval to game or main when needed. For accepted work, read actual changed cells on exit.",
        R"({"type":"object","properties":{"block":{"type":"string"},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"},"approval":{"type":"string","enum":["game","main"]}},"required":["block","x","y","z"],"additionalProperties":false})", SkillCategory::place, true, SkillKind::game, true},
    {"fill_target", "partner.fill_target", "Fill the already bound wall opening with a single full-block material from inventory. Supply only the material. Read the eventual per-cell result. An already correct cell is skipped. Unavailable or occupied cells stop the work without erasing earlier results.",
        R"({"type":"object","properties":{"block":{"type":"string"}},"required":["block"],"additionalProperties":false})", SkillCategory::place, true, SkillKind::motion},
    {"collect_confirmed", "partner.collect_confirmed", "Request player consent for a fixed collection. Route 'game' asks in the game; route 'main' lets the main Agent select a channel. The original collection runs only after consent.",
        R"({"type":"object","properties":{"block":{"type":"string"},"count":{"type":"integer"},"route":{"type":"string","enum":["game","main"]},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"required":["block","count"],"additionalProperties":false})", SkillCategory::dig_break, true, SkillKind::confirmed},
    {"task_update", "partner.task_update", "Update the shared task. Give target.message and a candidate ID from its playerFocus or exact cells for a coordinate instruction. For an original request to come over, register target.approach before moving to the player and bind cells with target.observation from a verified self view after arrival. Replacing a bound target requires a new player instruction and replace=true. Omit target to retain it, or use {} to clear it. Set collection=ground for exposed terrain away from structures.",
        R"({"type":"object","properties":{"goal":{"type":"string"},"detail":{"type":"string"},"next":{"type":"string","enum":["continue","scene","player","done"]},"collection":{"type":"string","enum":["ground","visible"]},"target":{"type":"object","properties":{"message":{"type":"integer"},"candidate":{"type":"string"},"approach":{"type":"boolean"},"replace":{"type":"boolean"},"observation":{"type":"string"},"cells":{"type":"array","items":{"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"required":["x","y","z"],"additionalProperties":false}}},"additionalProperties":false}},"required":["goal","next"],"additionalProperties":false})", SkillCategory::control, true, SkillKind::task},
    {"ask_player", "partner.ask_player", "Ask one natural in-game clarification if observation cannot resolve it. Optional options are the natural answers that match automatically. Read pendingQuestion with any free-form chat and do not assume consent or choose an uncertain target on silence. Set direct or main only for explicit escalation.",
        R"({"type":"object","properties":{"question":{"type":"string"},"options":{"type":"array","items":{"type":"string"}},"path":{"type":"string","enum":["local","direct","main"]}},"required":["question"],"additionalProperties":false})", SkillCategory::control, true, SkillKind::question},
    {"withdraw_confirmation", "partner.withdraw_confirmation", "Withdraw the waiting item from pendingApproval when the candidate is obsolete. Verify any new inventory or world state before deciding what to do next.",
        R"({"type":"object","properties":{"item":{"type":"integer"}},"required":["item"],"additionalProperties":false})", SkillCategory::control, true, SkillKind::approval_withdraw},
    {"skills", "partner.skills", "List Minecraft skill categories and availability. Request up to three category keys in categories to include their available tools with full parameters in the next decision.",
        R"({"type":"object","properties":{"categories":{"type":"array","items":{"type":"string"}}},"additionalProperties":false})", SkillCategory::query, true, SkillKind::catalog},
    {"knowledge", "partner.knowledge", "Look up versioned vanilla rules by block or item name, such as grass_block drops. Confirm quantities and changed cells using current game receipts.",
        R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"],"additionalProperties":false})", SkillCategory::query, false, SkillKind::knowledge}
}};
}
