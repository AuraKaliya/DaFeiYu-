#pragma once
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <string>
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
struct PartnerAction {
    std::string_view name, executor;
    SkillCategory category;
    bool plan = false, sequence = false;
    std::string_view plan_arguments = {};
    std::string_view plan_requirements = {};
    std::string_view sequence_arguments = {};
};

// 计划、顺序队列与直接工具统一查找实际执行技能，空执行器可结算已知但未实现的能力。
inline constexpr std::array partner_actions{
    PartnerAction{"observe","observe",SkillCategory::observation,true,false,"{} for a snapshot, or message with optional focus=hit or openings.",
        "Read only. Pointed observations require the original player message in the active world."},
    PartnerAction{"move","move",SkillCategory::movement,true,true,"Exact standing x,y,z. Exit verifies arrival.",{},"x,y,z, place or original message."},
    PartnerAction{"start_move","move",SkillCategory::movement},
    PartnerAction{"follow","follow",SkillCategory::movement,false,true,{},{},"Optional distance (2-5). Append or replace."},
    PartnerAction{"collect","collect",SkillCategory::dig_break,false,true,{},{},"x,y,z,block,count=1. Optional pickup on the step."},
    PartnerAction{"collect_confirmed","collect",SkillCategory::dig_break},
    PartnerAction{"dig_block","collect",SkillCategory::dig_break,true,false,"x,y,z,block with message for pointing or observation for self verify."},
    PartnerAction{"gather_ground","collect",SkillCategory::dig_break,true,true,"item, requiredStock, optional source block.",{},
        "item, requiredStock, optional source block."},
    PartnerAction{"place","place",SkillCategory::place,true,true,"x,y,z,block within the bound target. Exit reports changed cells.",{},
        "x,y,z,block within the bound target."},
    PartnerAction{"fill_target","fill_target",SkillCategory::place,true,false,"block for the bound cells. Exit reports each cell's result."},
    PartnerAction{"route","route",SkillCategory::movement,true,false,"Validated route.id and laps (1-8). A restart retains the original wall and lap count. Exit verifies laps."},
    PartnerAction{"craft","craft",SkillCategory::craft_process,true,false,"Plank item and desired new count (1-32), optional ingredient naming a log or stem.",
        "A prerequisite dig needs verified pickup. Check recipe, stock, space and workbench. Exit verifies consumption and output."},
};

// 顺序队列和能力目录使用相同的已开放分类规则。
inline bool sequence_available(const PartnerAction& action, const std::bitset<categories.size()>& enabled) {
    return action.sequence && (action.category == SkillCategory::movement || enabled.test(static_cast<std::size_t>(action.category)));
}

// 返回同一技能在各入口所用的实际执行能力。
inline const PartnerAction* partner_action(std::string_view name) noexcept {
    const auto action = std::ranges::find(partner_actions,name,&PartnerAction::name);
    return action == partner_actions.end() ? nullptr : &*action;
}

// 校验改动格和精确站位共用的整数坐标范围。
inline bool valid_action_cell(const nlohmann::json& args) {
    return std::ranges::all_of(std::array{"x","y","z"},[&](auto axis) {
        return args.contains(axis) && args.at(axis).is_number_integer() &&
            args.at(axis) >= -30000000 && args.at(axis) <= 30000000;
    });
}

// 核对顺序队列和计划中的目标库存及可选来源材料。
// 步骤：固定可用键 → 校验物品与来源名称 → 检查库存上限。
inline bool valid_stock_request(const nlohmann::json& args) {
    const auto name = [&](const char* key) {
        return args.contains(key) && args.at(key).is_string() && !args.at(key).get_ref<const std::string&>().empty() &&
            args.at(key).get_ref<const std::string&>().size() <= 90;
    };
    return args.size() == 2+args.count("block") && name("item") && (!args.contains("block") || name("block")) &&
        args.contains("requiredStock") && args.at("requiredStock").is_number_integer() &&
        args.at("requiredStock") >= 1 && args.at("requiredStock") <= 32;
}

// 把当前可编排的节点目录提供给首次选工具以及显式分类查询。
inline nlohmann::json plan_skills() {
    auto nodes = nlohmann::json::array();
    for (const auto& action : partner_actions)
        if (action.plan) nodes.push_back({{"skill",action.name},{"available",!action.executor.empty()}});
    return nodes;
}

struct PartnerSkill {
    std::string_view name, purpose, description, schema;
    SkillCategory category;
    bool changes;
    SkillKind kind = SkillKind::game;
    bool supports_approval = false;
};
inline constexpr std::array<PartnerSkill,18> partner_skills{{
    {"observe", "partner.observe", "Use {} for positions and inventory. Pointed mode uses message and focus=hit for the crosshair, objects for a cone plus labeled nearby blocks, or openings for wall gaps. "
        "Hit and objects share two attempts per message, reset on new events. Exhaustion returns hit_observation_limit. "
        "Wall mode returns wallSurvey from the original message and optional radius 5-13. Route mode takes observation=wallSurvey.id, optional wall and four ordered waypoints if supplied. "
        "For a bound wall, task.navigationWall.observation and id reference the survey and wall. A validated route returns route.id. "
        "Scene mode combines sources=self/player/message, message and focus for the original view, radius 1-5 and optional verify, detail or samples. "
        "A repeated scene query in the same tool batch returns observationWork referring to the original observation.id. "
        "Use live views' belowFeet for exact cells under actors, and self for verification. Samples count 1-3, intervalMs 100-1000. Search takes block and optional around.",
        R"({"type":"object","properties":{"mode":{"type":"string","enum":["pointed","scene","search","wall","route"]},"observation":{"type":"string"},"wall":{"type":"string"},"waypoints":{"type":"array","items":{"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"required":["x","y","z"],"additionalProperties":false}},"block":{"type":"string"},"around":{"type":"string","enum":["player","self"]},"message":{"type":"integer"},"focus":{"type":"string","enum":["hit","openings","objects"]},"sources":{"type":"array","items":{"type":"string","enum":["self","player","message"]}},"radius":{"type":"integer"},"detail":{"type":"string","enum":["summary","cells"]},"verify":{"type":"array","items":{"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"required":["x","y","z"],"additionalProperties":false}},"samples":{"type":"object","properties":{"count":{"type":"integer"},"intervalMs":{"type":"integer"}},"additionalProperties":false}},"additionalProperties":false})", SkillCategory::observation, false},
    {"move", "partner.move", "Walk without digging to a landmark, player (place=player), or integer x,y,z. An accepted result is a running work ID. Read actual arrival in the next activity context.",
        R"({"type":"object","properties":{"place":{"type":"string"},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"additionalProperties":false})", SkillCategory::movement, true},
    {"start_move", "partner.start_move", "Start moving toward one target: original message position (message), a landmark or current player (place), or xyz. Returns at acceptance when in flight. Movement continues while you listen and reply.",
        R"({"type":"object","properties":{"place":{"type":"string"},"message":{"type":"integer"},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"additionalProperties":false})", SkillCategory::movement, true, SkillKind::motion},
    {"follow", "partner.follow", "Keep following the bound player under the same work ID. A blocked route or missing entity waits for new evidence. Returns at acceptance. Optional distance is 2-5 blocks, default 3.",
        R"({"type":"object","properties":{"distance":{"type":"integer"}},"additionalProperties":false})", SkillCategory::movement, true, SkillKind::motion},
    {"stop_motion", "partner.stop_motion", "Cancel following by the stable following.work ID, or cancel the matching activeMotion work. Actual exit is reported separately.",
        R"({"type":"object","properties":{"work":{"type":"string"}},"required":["work"],"additionalProperties":false})", SkillCategory::control, true, SkillKind::motion_stop},
    {"queue_actions", "partner.queue_actions", "Submit a plan with up to four nodes, unique id and optional after IDs. Successors wait for their predecessors' verified results. "
        "See skills for node contracts and availability. A queued plan reports actual results by layer. Rejections report a reason and may include the node and retryAllowed. "
        "If a plan only prepares for the user's full goal, set remainingGoal to the outcome still needed after these steps. This does not grant further actions. "
        "Plan resolves is for one pointed dig_block awaiting a new target, including a saved afterPickup. Use task_update for other scoped clarifications. "
        "Append, interrupt and replace take sequence steps. Clear stops the plan and waiting actions.",
        R"({"type":"object","properties":{"operation":{"type":"string","enum":["append","clear","replace","interrupt","plan"]},"work":{"type":"string"},"remainingGoal":{"type":"string"},"resolves":{"type":"integer"},"steps":{"type":"array","items":{"type":"object","properties":{"id":{"type":"string"},"after":{"type":"array","items":{"type":"string"}},"skill":{"type":"string"},"pickup":{"type":"string"},"arguments":{"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"},"message":{"type":"integer"},"observation":{"type":"string"},"focus":{"type":"string","enum":["hit","openings"]},"place":{"type":"string"},"distance":{"type":"integer"},"block":{"type":"string"},"count":{"type":"integer"},"item":{"type":"string"},"ingredient":{"type":"string"},"requiredStock":{"type":"integer"},"route":{"type":"string"},"laps":{"type":"integer"}},"additionalProperties":false}},"required":["skill","arguments"],"additionalProperties":false}}},"required":["operation"],"additionalProperties":false})", SkillCategory::control, true, SkillKind::action_queue},
    {"collect", "partner.collect", "Gather visible blocks by Minecraft registry name and count (1-32), within the task's collection scope. For a specific block supply x,y,z and count=1. "
        "Set approval to game or main when consent is needed. Delegated collection requires player consent and defaults to main. Exit reports cell and inventory changes separately.",
        R"({"type":"object","properties":{"block":{"type":"string"},"count":{"type":"integer"},"approval":{"type":"string","enum":["game","main"]},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"required":["block","count"],"additionalProperties":false})", SkillCategory::dig_break, true, SkillKind::game, true},
    {"dig_block", "partner.dig_block", "Dig exact x,y,z with the expected block. Use message for a pointed target, or observation.id from self verify or self.belowFeet for a self-read cell. "
        "For dig_stand_support move safely first and reverify the same cell. For a material mismatch check what the player meant before changing the target or block. "
        "Use resolves for a pending pointed question. A saved successor requires a plan. Set editBuilt with approval to change completed structures. Exit reports block and inventory changes.",
        R"({"type":"object","properties":{"block":{"type":"string"},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"},"approval":{"type":"string","enum":["game","main"]},"editBuilt":{"type":"boolean"},"message":{"type":"integer"},"observation":{"type":"string"},"resolves":{"type":"integer"}},"required":["block","x","y","z"],"additionalProperties":false})", SkillCategory::dig_break, true, SkillKind::motion, true},
    {"gather_ground", "partner.gather_ground", "Bring inventory of item up to requiredStock (1-32) from exposed ground. Use the total you need. Set block only to restrict the search to one permitted source. Stops on enough stock or unverified pickup.",
        R"({"type":"object","properties":{"block":{"type":"string"},"item":{"type":"string"},"requiredStock":{"type":"integer"},"approval":{"type":"string","enum":["game","main"]}},"required":["item","requiredStock"],"additionalProperties":false})", SkillCategory::dig_break, true, SkillKind::motion, true},
    {"reply", "partner.reply", "Send one line to the bound player and return its delivery receipt.",
        R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"],"additionalProperties":false})", SkillCategory::communication, true},
    {"place", "partner.place", "Place an inventory block at an exact empty x,y,z cell within the bound task target. For a door choose the lower cell. "
        "Set approval to game or main when needed. Exit reports the actual changed cells and material use.",
        R"({"type":"object","properties":{"block":{"type":"string"},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"},"approval":{"type":"string","enum":["game","main"]}},"required":["block","x","y","z"],"additionalProperties":false})", SkillCategory::place, true, SkillKind::game, true},
    {"fill_target", "partner.fill_target", "Fill the bound opening with one full-block inventory material. Read each cell's actual result. Already correct cells are skipped, cells missing a usable face may be retried after others, and occupied cells stop the work with earlier results intact.",
        R"({"type":"object","properties":{"block":{"type":"string"}},"required":["block"],"additionalProperties":false})", SkillCategory::place, true, SkillKind::motion},
    {"collect_confirmed", "partner.collect_confirmed", "Request player consent for a fixed collection. Route 'game' asks in the game; route 'main' lets the main Agent select a channel. The original collection runs only after consent.",
        R"({"type":"object","properties":{"block":{"type":"string"},"count":{"type":"integer"},"route":{"type":"string","enum":["game","main"]},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"required":["block","count"],"additionalProperties":false})", SkillCategory::dig_break, true, SkillKind::confirmed},
    {"task_update", "partner.task_update", "Record goal, scope and revision. start_task takes current message, kind and goal. restart_task takes current message and task ID. "
        "begin_correction takes current message. commit_correction takes that message, goal, next and target with source message and current self observation. "
        "Use target.candidate for one opening or target.candidates for multiple authorized openings in the same original observation. For multiple openings or self-selected cells, verify all cells and include target.observation. "
        "With the current player's natural answer, resolves closes the matching local scoped question. target.approach waits for arrival and self verify before binding. "
        "target.replace requires a new player instruction. Omit operation for goal/next updates and target for notes. Use target={} to release. Collection=ground limits gathering to exposed terrain.",
        R"({"type":"object","properties":{"operation":{"type":"string","enum":["begin_correction","commit_correction","start_task","restart_task"]},"message":{"type":"integer"},"task":{"type":"integer"},"kind":{"type":"string","enum":["navigation","work"]},"resolves":{"type":"integer"},"goal":{"type":"string"},"detail":{"type":"string"},"next":{"type":"string","enum":["continue","scene","player","done"]},"collection":{"type":"string","enum":["ground","visible"]},"target":{"type":"object","properties":{"message":{"type":"integer"},"candidate":{"type":"string"},"candidates":{"type":"array","items":{"type":"string"}},"approach":{"type":"boolean"},"replace":{"type":"boolean"},"observation":{"type":"string"},"cells":{"type":"array","items":{"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"required":["x","y","z"],"additionalProperties":false}}},"additionalProperties":false}},"additionalProperties":false})", SkillCategory::control, true, SkillKind::task},
    {"ask_player", "partner.ask_player", "Register a clarification and return its item ID. Local placementOptions take 2-4 distinct reply labels with their already bound cells and exclude options and afterPickup. "
        "Matching local labels are accepted within the answer window. Verify questionResult.placementCells against the current scene before placing. placementExcluded lists unselected cells. "
        "Use local scope for in-game questions about collect, place or craft, with optional block or item. Exact options are matched automatically; use task_update resolves with natural scoped answers. Leave unrelated steps available. "
        "Local afterPickup records a known output item and count (1-32), with optional required pickup. Use item=planks when the wood type is still unknown. "
        "Use direct or main for explicit escalation.",
        R"({"type":"object","properties":{"question":{"type":"string"},"options":{"type":"array","items":{"type":"string"}},"scope":{"type":"object","properties":{"skill":{"type":"string","enum":["collect","place","craft"]},"block":{"type":"string"},"item":{"type":"string"}},"required":["skill"],"additionalProperties":false},"placementOptions":{"type":"array","items":{"type":"object","properties":{"answer":{"type":"string"},"cells":{"type":"array","items":{"type":"object","properties":{"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"required":["x","y","z"],"additionalProperties":false}}},"required":["answer","cells"],"additionalProperties":false}},"path":{"type":"string","enum":["local","direct","main"]},"afterPickup":{"type":"object","properties":{"item":{"type":"string"},"count":{"type":"integer"},"pickup":{"type":"string"}},"required":["item","count"],"additionalProperties":false}},"required":["question"],"additionalProperties":false})", SkillCategory::control, true, SkillKind::question},
    {"withdraw_confirmation", "partner.withdraw_confirmation", "Withdraw the waiting item from pendingApproval when the candidate is obsolete. Verify any new inventory or world state before deciding what to do next.",
        R"({"type":"object","properties":{"item":{"type":"integer"}},"required":["item"],"additionalProperties":false})", SkillCategory::control, true, SkillKind::approval_withdraw},
    {"skills", "partner.skills", "List Minecraft skills by entry. A category can have plan nodes even without standalone tools. "
        "Contracts report current tool visibility and execution requirements. Request up to three category keys for plan parameters and to expose newly available standalone tools in the next decision.",
        R"({"type":"object","properties":{"categories":{"type":"array","items":{"type":"string"}}},"additionalProperties":false})", SkillCategory::query, true, SkillKind::catalog},
    {"knowledge", "partner.knowledge", "Look up versioned vanilla rules by block or item name, such as grass_block drops. Confirm quantities and changed cells using current game receipts.",
        R"({"type":"object","properties":{"query":{"type":"string"}},"required":["query"],"additionalProperties":false})", SkillCategory::query, false, SkillKind::knowledge}
}};

// 为共享队列入口生成和实际校验一致的分操作技能目录。
// 步骤：提取计划能力和顺序能力 → 标记尚不可执行的节点 → 固定 ABI 所需的字符串寿命。
inline std::string_view partner_schema(const PartnerSkill& skill) {
    if (skill.kind != SkillKind::action_queue) return skill.schema;
    static const auto schema = [&skill] {
        std::string description = "Plan node parameters:";
        for (const auto& action : partner_actions) if (action.plan) {
            description += "\n"+std::string(action.name);
            if (action.executor.empty()) description += " (unavailable)";
            description += ": "+std::string(action.plan_arguments);
            if (!action.plan_requirements.empty()) description += " "+std::string(action.plan_requirements);
        }
        description += "\nFor append, interrupt or replace:";
        for (const auto& action : partner_actions) if (action.sequence) description += " "+std::string(action.name);
        description += ". Interrupt cannot start a new follow.";
        std::string sequence = "Ordinary sequence parameters:";
        for (const auto& action : partner_actions) if (action.sequence)
            sequence += "\n"+std::string(action.name)+": "+std::string(action.sequence_arguments);
        auto value = nlohmann::json::parse(skill.schema);
        auto& fields = value["properties"];
        fields["resolves"]["description"] = "Pending question item for a newer verified dig. Supply one dig_block node with the answer message; any saved successor is retained.";
        auto& node = fields["steps"]["items"]["properties"];
        node["skill"]["description"] = std::move(description);
        node["arguments"]["description"] = std::move(sequence);
        node["pickup"]["description"] = "For a dig_block plan node or an ordinary collect step, the item that must actually enter inventory before the successor can advance.";
        return value.dump();
    }();
    return schema;
}
}
