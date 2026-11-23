#include "companion_host.hpp"
#include "application.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>

namespace {
// 从唯一结算对象读取主工作回执，兼容组内还有未执行的后继。
const nlohmann::json& primary_receipt(const nlohmann::json& input) {
    const auto& settlement = input.at("workSettlement");
    return settlement.at("receipts").at(settlement.at("primary").get<std::size_t>());
}

// 体验服用已授权的单一查看参数调用正式工具，游戏事实只取自插件回执。
dfy::model::Response sample_turn(const dfy::model::Request& request) {
    using namespace dfy::model;
    const auto& last = request.messages.back();
    if (last.role == Role::tool || request.tools.size() != 1) return {{Role::assistant, "本轮查看已结束。"}};
    const auto newline = last.content.rfind('\n');
    if (newline == std::string::npos) return {{Role::assistant, "我在这里。"}};
    const auto args = nlohmann::json::parse(last.content.substr(newline + 1), nullptr, false);
    if (!args.is_object() || !args.contains("order") || !args.contains("place")) return {{Role::assistant, "我在这里。"}};
    return {{Role::assistant, "", {{"visit", request.tools.front().name, args.dump()}}}, FinishReason::tool_calls};
}
// 仅向离线探针记下真实进入后端的事实和替身选中的结果，供外部玩家对照。
void record_input(const std::filesystem::path& root, const nlohmann::json& record) {
    static std::mutex lock;
    std::lock_guard guard(lock);
    std::ofstream(root / "model-events.jsonl", std::ios::app | std::ios::binary) << record.dump() << '\n';
}

// 在固定外部输入下提供不同可控选择；输出文字不参与动作分支判定。
// 步骤：记录原始事件 → 选择继续或停止在途工作 → 按实际工具回执决定行动及是否发言。
dfy::model::Response partner_sample(const dfy::model::Request& request, const std::filesystem::path& root) {
    using namespace dfy::model;
    using Json = nlohmann::json;
    const auto& last = request.messages.back();
    const auto speak = [](std::string message) {
        return Response{{Role::assistant, std::move(message)}, FinishReason::stop};
    };
    const auto internal = [&](std::string text) { return speak(Json{{"internal",text}}.dump()); };
    const std::string reply = "我在呢，就在你下面不远的地方。看你站在上面一层，我这边是山脚附近，有块小台地可以上去。要不要我上去跟你汇合？";
    const auto call = [&](const std::string& skill, const Json& arguments) -> Response {
        const auto tool = std::ranges::find_if(request.tools, [&](const auto& def) { return def.name.ends_with("_" + skill); });
        if (tool == request.tools.end()) throw std::runtime_error("missing registered skill: " + skill);
        if (skill != "skills" && skill != "stop_motion") record_input(root, {{"call",skill},{"arguments",arguments}});
        return {{Role::assistant, "", {{skill, tool->name, arguments.dump()}}}, FinishReason::tool_calls};
    };
    const auto has = [&](const std::string& skill) {
        return std::ranges::any_of(request.tools,[&](const auto& tool) { return tool.name.ends_with("_" + skill); });
    };
    if (!has("skills") || !has("observe") || !has("reply") || request.messages.front().role != Role::system)
        throw std::runtime_error("invalid partner scope");
    if (last.role == Role::user) {
        const auto input = Json::parse(last.content);
        record_input(root, input);
        if (input.at("trigger") == "motion.finished") return internal("已读实际退出");
        const bool expanded = input.at("trigger") == "skills.expanded";
        const auto& event = input.at(expanded ? "recentEvents" : "events").back();
        if (event.at("type") != "player.message") return internal("暂时安静");
        const auto seq = event.at("sequence").get<unsigned>();
        if (seq == 2) return call("observe", {{"around","player"}});
        if (seq == 5 || seq == 7) {
            const auto& work = input.at("activeMotion");
            const bool stop = work.at("order") == 6;
            record_input(root, {{"check",input.at("events")},{"work",work},{"stop",stop}});
            return stop ? call("stop_motion",{{"work",work.at("work")}}) : internal("继续原动作");
        }
        if (seq == 3) {
            if (!expanded) return call("observe", {{"block","dirt"}});
            if (!input.at("task").at("lastAction").at("result").at("visible").empty())
                return call("collect", {{"block","dirt"},{"count",2}});
        }
        if (seq == 4 || seq == 6) {
            if (!has("start_move")) return call("skills",{{"categories",{"movement"}}});
            return call("start_move", {{"x",seq == 4 ? 32 : 1},{"y",-60},{"z",-4}});
        }
        if (seq == 8) throw std::runtime_error("fixture provider unavailable");
        return internal("等待");
    }
    if (last.tool_call_id == "skills") return internal("已选择下一轮工具");
    if (last.tool_call_id == "reply") return speak(reply);
    if (last.tool_call_id == "stop_motion") return call("reply",{{"text",reply}});
    const auto result = Json::parse(last.content.substr(last.content.find('{')));
    if (result.at("status") != "done") return internal("已读工具结果");
    if (last.tool_call_id == "observe" && result.contains("survey")) return speak(reply);
    if (last.tool_call_id == "observe" && !result.at("visible").empty()) {
        if (has("collect")) return call("collect", {{"block","dirt"},{"count",2}});
        return call("skills", {{"categories",{"dig_break"}}});
    }
    return internal("已读工作回执");
}

// 从原观察网格读取一个已知坐标，缺格无法用样本中的预期值补齐。
std::string survey_block(const nlohmann::json& survey, int x, int y, int z) {
    const auto& origin = survey.at("origin");
    const auto row = survey.at("layers").at(y-origin.at("y").get<int>()).at(z-origin.at("z").get<int>()).get<std::string>();
    const std::string glyph(1,row.at(x-origin.at("x").get<int>()));
    return survey.at("palette").value(glyph,glyph);
}

// 固定边走边聊的模型输出，用原工作和插件分配的修订校验跨轮上下文。
// 步骤：保存跟随意图并启动 → 在两个玩家回合分别发言 → 明确停止并在实际退出后结束任务。
dfy::model::Response follow_sample(const dfy::model::Request& request) {
    using namespace dfy::model;
    using Json = nlohmann::json;
    const auto call = [&](const std::string& skill, const Json& args) -> Response {
        const auto tool = std::ranges::find_if(request.tools,[&](const auto& def) { return def.name.ends_with("_"+skill); });
        if (tool == request.tools.end()) throw std::runtime_error("follow probe: missing "+skill);
        if (skill == "task_update" && Json::parse(tool->parameters).at("properties").contains("revision"))
            throw std::runtime_error("follow probe: model should not supply task revision");
        return {{Role::assistant,"",{{skill,tool->name,args.dump()}}},FinishReason::tool_calls};
    };
    const auto& last = request.messages.back();
    if (last.role == Role::tool && last.tool_call_id == "queue_actions") {
        const auto result = Json::parse(last.content.substr(last.content.find('{')));
        const auto input = Json::parse(request.messages.at(1).content);
        const auto text = input.at("events").back().at("text");
        if (text == "到哪了" && result.at("status") == "queued")
            return call("reply",{{"text","我还在跟着你。"}});
        if (text == "继续边走边聊" && result.at("status") == "cleared")
            return {{Role::assistant,"我继续跟着你。"}};
        return {{Role::assistant,R"({"internal":"queued replacement"})"}};
    }
    if (last.role == Role::tool && last.tool_call_id == "task_update") {
        const auto result = Json::parse(last.content.substr(last.content.find('{')));
        const auto& task = result.at("task");
        if (task.at("revision") == 1) return call("follow",Json::object());
        if (task.at("revision") != 2 || task.at("next") != "done") throw std::runtime_error("follow probe: revision did not advance");
    }
    if (last.role == Role::user) {
        const auto input = Json::parse(last.content);
        if (input.at("trigger") == "motion.finished") {
            const auto& work = primary_receipt(input);
            if (work.at("skill") == "move") return {{Role::assistant,R"({"internal":"replacement finished"})"}};
            if (work.at("skill") != "follow" || work.at("order") != 2 || work.at("result").at("status") != "interrupted")
                throw std::runtime_error("follow probe: actual exit missing from the task");
            return call("task_update",{{"goal","跟着玩家走"},{"next","done"}});
        }
        const auto& message = input.at("events").back();
        if (message.at("type") == "player.message") {
            const auto text = message.at("text");
            if (text == "你跟着我走") return call("task_update",{{"goal","跟着玩家走"},{"next","player"}});
            const auto& motion = input.at("activeMotion");
            if (motion.at("order") != 2 || motion.at("skill") != "follow" || input.at("task").at("revision") != 1)
                throw std::runtime_error("follow probe: original work or task revision lost");
            const Json steps = Json::array({{{"skill","move"},{"arguments",{{"x",-6},{"y",-60},{"z",-6}}}}});
            if (text == "到哪了") {
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
                return call("queue_actions",{{"operation","append"},{"steps",steps}});
            }
            if (text == "继续边走边聊") {
                if (input.at("queuedActions").size() != 1) throw std::runtime_error("follow probe: waiting step lost");
                return call("queue_actions",{{"operation","clear"}});
            }
            if (text == "先别跟了") return call("queue_actions",{{"operation","replace"},{"work",motion.at("work")},{"steps",steps}});
        }
    }
    return {{Role::assistant,R"({"internal":"processed"})"}};
}

// 在真实场地用固定响应验证取材、垫高和原跟随恢复。
// 步骤：开始跟随并说明受阻 → 按库存分流取材 → 观察并绑定支撑点 → 按实际站位答复。
dfy::model::Response layered_follow_sample(const dfy::model::Request& request, bool resupply) {
    using namespace dfy::model;
    using Json = nlohmann::json;
    const Json pads = Json::array({{{"x",32},{"y",-60},{"z",-13}},{{"x",33},{"y",-59},{"z",-13}}});
    const Json sources = Json::array({{{"x",30},{"y",-60},{"z",-16}},{{"x",31},{"y",-60},{"z",-16}}});
    const auto call = [&](std::string_view skill, std::string id, const Json& args) {
        const auto suffix = "_"+std::string(skill);
        const auto tool = std::ranges::find_if(request.tools,[&](const auto& item) { return item.name.ends_with(suffix); });
        if (tool == request.tools.end()) throw std::runtime_error("layered follow probe: tool missing");
        return ToolCall{std::move(id),tool->name,args.dump()};
    };
    const auto invoke = [&](std::string_view skill, std::string id, const Json& args) -> Response {
        return {{Role::assistant,"",{call(skill,std::move(id),args)}},FinishReason::tool_calls};
    };
    const auto& last = request.messages.back();
    if (last.role == Role::tool) {
        if (last.tool_call_id == "start-steps") return
            invoke("observe","step-survey",{{"sources",{"self"}},{"verify",pads}});
        if (last.tool_call_id == "material-survey") {
            const auto result = Json::parse(last.content.substr(last.content.find('{')));
            const auto& observation = result.at("observation"), &sample = observation.at("samples").back();
            if (sample.at("views").front().at("actor").at("inventory").value("dirt",0) != 0)
                throw std::runtime_error("resupply probe: the actor already had dirt");
            Json steps = Json::array();
            for (std::size_t i = 0; i < sources.size(); ++i) {
                const auto& verified = sample.at("verified").at(i);
                if (verified.at("atSelf") != true || verified.at("block") != "dirt")
                    throw std::runtime_error("resupply probe: the source cells were not read back");
                auto candidate = sources.at(i);
                candidate["block"] = "dirt";
                candidate["observation"] = observation.at("id");
                steps.push_back({{"id","material-"+std::to_string(i+1)},{"skill","dig_block"},{"pickup","dirt"},
                    {"arguments",std::move(candidate)}});
            }
            return invoke("queue_actions","gather-materials",{{"operation","plan"},{"remainingGoal","垫两步靠近高处玩家"},
                {"steps",std::move(steps)}});
        }
        if (last.tool_call_id == "step-survey") {
            const auto result = Json::parse(last.content.substr(last.content.find('{')));
            const auto& observation = result.at("observation");
            const auto& cells = observation.at("samples").back().at("verified");
            if (cells.size() != 2 || std::ranges::any_of(cells,[](const auto& cell) {
                return cell.at("atSelf") != true || cell.at("block") != "air";
            })) throw std::runtime_error("layered follow probe: pads were missing from the real observation");
            return invoke("task_update","bind-steps",{{"goal","垫两步靠近高处玩家"},{"next","player"},
                {"target",{{"message",4},{"observation",observation.at("id")},{"cells",pads}}}});
        }
        if (last.tool_call_id == "bind-steps") {
            const auto result = Json::parse(last.content.substr(last.content.find('{')));
            if (result.at("status") != "updated") throw std::runtime_error("layered follow probe: target not bound");
            auto first = pads.at(0), second = pads.at(1), above_first = first, above_second = second;
            first["block"] = second["block"] = "dirt";
            above_first["y"] = -59;
            above_second["y"] = -58;
            const Json steps = Json::array({{{"id","place-first"},{"skill","place"},{"arguments",first}},
                {{"id","stand-first"},{"skill","move"},{"arguments",above_first},{"after",{"place-first"}}},
                {{"id","place-second"},{"skill","place"},{"arguments",second},{"after",{"stand-first"}}},
                {{"id","stand-second"},{"skill","move"},{"arguments",above_second},{"after",{"place-second"}}}});
            return {{Role::assistant,"",{call("queue_actions","steps",{{"operation","plan"},{"steps",steps}}),
                call("reply","starting",{{"text","我垫两步上来。"}})}},FinishReason::tool_calls};
        }
        return {{Role::assistant,R"({"internal":"probe settlement"})"}};
    }
    const auto input = Json::parse(last.content);
    const auto& trigger = input.at("trigger");
    if (trigger == "follow.attention") return {{Role::assistant,"上面有点高，我先找找路。"}};
    if (trigger == "plan.finished") {
        const auto& layers = input.at("planSettlements");
        if (resupply && layers.size() == 1) {
            const auto& plan = input.at("taskPlan");
            if (plan.at("origin").at("sequence") != 4 || plan.at("remainingGoal") != "垫两步靠近高处玩家" ||
                input.at("task").contains("id"))
                throw std::runtime_error("resupply probe: the plan lost the unregistered goal or original message");
            const auto& nodes = layers.front().at("nodes");
            if (nodes.size() != 2 || std::ranges::any_of(nodes,[](const auto& node) {
                const auto& result = node.at("result");
                return node.at("skill") != "dig_block" || result.at("status") != "done" || result.at("inventoryDelta").value("dirt",0) != 1;
            })) throw std::runtime_error("resupply probe: material pickup was not confirmed");
            return invoke("observe","step-survey",{{"sources",{"self"}},{"verify",pads}});
        }
        if (layers.size() != 4 || input.at("taskPlan").at("status") != "done")
            throw std::runtime_error("layered follow probe: incomplete pad work");
        if (resupply && (input.at("taskPlan").at("origin").at("sequence") != 4 ||
            input.at("taskPlan").at("order") != 5 || input.at("taskPlan").at("supersedes") == 0))
            throw std::runtime_error("resupply probe: the progress chat replaced the original request after binding the pads");
        return {{Role::assistant,"我踩到第二格了。"}};
    }
    const auto& events = input.at("events");
    if (events.empty()) return {{Role::assistant,R"({"internal":"probe settlement"})"}};
    const auto text = events.back().value("text","");
    if (text == "大肥鱼，跟着我") return {{Role::assistant,"",{call("follow","follow",Json::object()),
        call("reply","following",{{"text","跟着呢。"}})}},FinishReason::tool_calls};
    if (text == "能听见吗") return {{Role::assistant,"听得见。"}};
    if (text == "我在上面等你" && input.at("taskPlan").at("origin").at("sequence") == 4)
        return {{Role::assistant,"收到，我在取材。"}};
    if (resupply && text == "你挖俩方块垫一下") {
        if (request.messages.front().content.find("nearbyMaterials") == std::string::npos)
            throw std::runtime_error("resupply probe: material guidance was missing before selecting a block");
        return invoke("observe","material-survey",{{"sources",{"self"}},{"radius",5},{"verify",sources}});
    }
    if (text == "你可以垫上来")
        return invoke("task_update","start-steps",{{"operation","start_task"},{"message",4},{"kind","work"},{"goal","垫两步靠近高处玩家"}});
    if (text == "不用跟了") return invoke("stop_motion","stop-follow",{{"work",input.at("following").at("work")}});
    return {{Role::assistant,R"({"internal":"probe settlement"})"}};
}

// 固定主回合只能从实际收信提供的工具中选择渠道或回信。
dfy::model::Response main_ask_sample(const dfy::model::Request& request, bool reply) {
    using namespace dfy::model;
    using Json = nlohmann::json;
    const auto& last = request.messages.back();
    if (last.role == Role::tool) return {{Role::assistant, "主回合已处理原信。"}};
    const auto envelope = Json::parse(last.content);
    const auto body = Json::parse(envelope.at("content").get<std::string>());
    if (envelope.at("category") != (reply ? "interaction.ask.completed" : "mc.partner.question") ||
        !body.contains("item") || !body.contains("eventDomain") || !body.contains("event") ||
        (reply && body.at("answer") != "靠河那段，留木门"))
        throw std::runtime_error("wall probe: main call lost original envelope");
    return {{Role::assistant,"",{{"route",reply ? "mail_reply" : "ask",reply ? "{}" : R"({"channel":"game"})"}}},
        FinishReason::tool_calls};
}

// 用固定模型验证墙体目标跨回合、观察、范围展开和原候选确认。
// 步骤：保存原分工及指向 → 读取旧位置并按需澄清 → 处理材料变更 → 补原缺格或继续门洞探针。
dfy::model::Response wall_sample(const dfy::model::Request& request, const std::filesystem::path& root, std::string_view ask_path,
    bool gap_only) {
    using namespace dfy::model;
    using Json = nlohmann::json;
    const auto main = std::ranges::find_if(request.tools,[](const auto& tool) { return tool.name == "ask" || tool.name == "mail_reply"; });
    if (main != request.tools.end()) return main_ask_sample(request,main->name == "mail_reply");
    const auto& last = request.messages.back();
    const auto internal = [](std::string text) { return Response{{Role::assistant,Json{{"internal",std::move(text)}}.dump()}}; };
    const auto call = [&](std::string skill, std::string id, const Json& args) -> Response {
        const auto found = std::ranges::find_if(request.tools,[&](const auto& tool) { return tool.name.ends_with("_"+skill); });
        if (found == request.tools.end()) throw std::runtime_error("wall probe: skill outside current scope: "+skill);
        record_input(root,{{"call",skill},{"id",id},{"arguments",args}});
        return {{Role::assistant,"",{{std::move(id),found->name,args.dump()}}},FinishReason::tool_calls};
    };
    if (request.tools.empty()) return {{Role::assistant,"continue"}};
    const bool ambiguous = ask_path == "local";
    const auto survey_original = [&] { return call("observe","survey",{{"message",3}}); };
    // 限制补墙使用游戏生成的旧消息候选。
    const auto bind_gap = [&] {
        return call("task_update","bind",{{"goal","continue existing two-layer dirt wall and door"},{"next","player"},
            {"target",{{"message",3},{"candidate","3,-59,-3:x"}}}});
    };
    // 输入中的原话位置只用于走近，实际站位必须与当时的方块高度对应。
    const auto check_arrival = [&](const Json& result) {
        if (result.at("status") != "done" || result.at("sourceMessage") != 3 ||
            std::floor(result.at("position").at("y").get<double>()) != -60)
            throw std::runtime_error("gap probe: movement did not return a stand at the original message height");
        return survey_original();
    };
    // 收到放置读回后才开始门洞提议，立即退出和在途工作读回使用同一任务变更。
    const auto plan_door = [&] {
        if (gap_only) return call("task_update","complete",{{"goal","continue existing two-layer dirt wall and door"},{"next","done"}});
        return call("task_update","door-plan",{{"goal","put the agreed door in its opening"},
            {"next","player"},{"target",Json::object()}});
    };
    if (last.role == Role::tool) {
        if (last.tool_call_id == "approval-bypass")
            return call("reply","received-material",{{"text","收到泥土，我补刚才那处。"}});
        const auto result = Json::parse(last.content.substr(last.content.find('{')));
        if (last.tool_call_id == "save" || last.tool_call_id == "point" || last.tool_call_id == "directory" ||
            last.tool_call_id == "question") return internal("fixed round done");
        if (last.tool_call_id == "approach") {
            if (result.at("status") == "accepted") return internal("walking to the original gap");
            return check_arrival(result);
        }
        if (last.tool_call_id == "survey" || last.tool_call_id == "resurvey") {
            const auto& survey = result.at("survey");
            if (survey_block(survey,2,-59,-3) != "dirt" || survey_block(survey,3,-59,-3) != "." ||
                survey_block(survey,3,-60,-3) != "dirt" || survey_block(survey,7,-59,-3) != "." ||
                result.at("anchor").at("message") != 3 || result.at("anchor").at("position").at("x") >= 4 ||
                result.at("player").at("x") < 7)
                throw std::runtime_error("wall probe: survey lost the original gap or the separate live player position");
            const auto& focus = result.at("playerFocus");
            if ((ambiguous ? focus.at("status") != "ambiguous" || !focus.at("choice").is_null() :
                focus.at("choice") != "3,-59,-3:x") || !std::ranges::any_of(focus.at("candidates"),[](const auto& gap) {
                return gap.at("id") == "7,-60,-3:x" && gap.at("height") == 2;
            })) throw std::runtime_error("wall probe: original player view did not identify gap A with a separate gap B");
            if (ambiguous && last.tool_call_id == "survey")
                return call("ask_player","question",{{"question","你指靠河的那段，还是靠木屋的那段？"},
                    {"options",{"靠河那段，留木门","靠木屋那段"}}});
            if (!ask_path.empty() && last.tool_call_id == "survey")
                return call("ask_player","question",{{"question","你指靠河的那段，还是靠木屋的那段？"},{"path",ask_path}});
            if (gap_only && !ambiguous) return call("observe","current-view",{{"around","player"},{"block","oak_door"}});
            return bind_gap();
        }
        if (last.tool_call_id == "current-view") {
            const auto& search = result.at("blockSearch");
            const Json door{{"id","4,-60,2"},{"block","oak_door"},{"status","complete"},
                {"lower",{{"x",4},{"y",-60},{"z",2}}},{"upper",{{"x",4},{"y",-59},{"z",2}}}};
            if (result.contains("anchor") || result.at("player").at("x") < 7 ||
                search.at("center").at("x") != 7 || search.at("block") != "oak_door" ||
                survey_block(result.at("survey"),7,-59,-3) != "." ||
                result.at("doors") != Json::array({door}))
                throw std::runtime_error("gap probe: live search did not distinguish the existing door from the opening");
            return bind_gap();
        }
        if (last.tool_call_id == "gather" && (result.at("status") != "waiting" || !result.contains("item")))
            throw std::runtime_error("gap probe: collection did not enter the waiting state");
        if (last.tool_call_id == "stock") {
            if (result.at("inventory").value("dirt",0) != 4) throw std::runtime_error("gap probe: new dirt was not read back");
            const auto original = Json::parse(request.messages.at(1).content);
            return call("withdraw_confirmation","withdraw",{{"item",original.at("pendingApproval").at("item")}});
        }
        if (last.tool_call_id == "withdraw" && result.at("status") == "withdrawn")
            return call("collect","approval-bypass",{{"block","minecraft:dirt"},{"count",1}});
        if (last.tool_call_id == "received-material" && result.at("sent") == true)
            return call("place","wrong-gap",{{"block","dirt"},{"x",7},{"y",-60},{"z",-3}});
        if (last.tool_call_id == "wrong-gap" && result.at("status") == "blocked" && result.at("placementAttempted") == false)
            return call("place","wall",{{"block","dirt"},{"x",3},{"y",-59},{"z",-3}});
        if (last.tool_call_id == "wall" && result.at("status") == "done" && result.at("placed") == true)
            return plan_door();
        if (last.tool_call_id == "door-plan" && result.at("status") == "updated")
            return call("task_update","bind-door",{{"goal","put the agreed door in its opening"},{"next","player"},
                {"target",{{"message",4},{"cells",Json::array({{{"x",7},{"y",-60},{"z",-3}},
                    {{"x",7},{"y",-59},{"z",-3}}})}}}});
        if (last.tool_call_id == "bind-door" && result.at("status") == "updated")
            return call("place","door",{{"block","oak_door"},{"x",7},{"y",-60},{"z",-3},{"approval","game"}});
        return internal("fixed action ended");
    }
    const auto input = Json::parse(last.content);
    record_input(root,input);
    if (request.messages.size() != 2) throw std::runtime_error("wall probe: older raw context was sent again");
    if (input.at("trigger") == "motion.finished") {
        const auto& finished = primary_receipt(input);
        if (gap_only && finished.at("skill") == "move") return check_arrival(finished.at("result"));
        if (finished.at("skill") == "place" && finished.at("result").value("placed",false) &&
            input.at("task").at("goal") == "continue existing two-layer dirt wall and door") return plan_door();
        return internal("fixed action ended");
    }
    if (input.at("trigger") == "approval.result") return internal("approval settled without an action");
    if (input.at("trigger") == "skills.expanded") {
        const auto& task = input.at("task");
        if (!task.at("target").contains("cells")) {
            if (std::ranges::any_of(request.tools,[](const auto& tool) { return tool.name.ends_with("_place"); }))
                throw std::runtime_error("wall probe: place granted before selecting a target");
            return internal("原位置已知，等玩家指定目标");
        }
        const auto& observation = task.at("lastAction").at("result");
        if (gap_only && !ambiguous && (observation.contains("anchor") ||
            task.at("targetObservation").at("message") != 3 ||
            task.at("targetObservation").at("playerFocus").at("choice") != "3,-59,-3:x"))
            throw std::runtime_error("gap probe: the live observation replaced the original pointing evidence");
        if (task.at("goal") != "continue existing two-layer dirt wall and door" || task.at("target").at("message") != 3 ||
            task.at("detail") != "Find the player's original wall and leave the intended doorway" ||
            !observation.value("surveyAvailable",false) || observation.contains("survey") || !observation.contains("sampledAtMs") ||
            !input.at("events").empty() || input.at("recentDialogue").front().at("order") != 2)
            throw std::runtime_error("wall probe: expansion lost prior goal or environment");
        if (ambiguous) {
            if (std::ranges::any_of(request.tools,[](const auto& tool) { return tool.name.ends_with("_collect"); }))
                return call("collect","gather",{{"block","dirt"},{"count",1},{"approval","game"}});
            return call("skills","directory",{{"categories",{"dig_break"}}});
        }
        return call("place","wrong-gap",{{"block","dirt"},{"x",7},{"y",-60},{"z",-3}});
    }
    if (input.contains("questionResult") && !input.at("task").at("target").contains("cells") &&
        input.at("task").at("goal") == "continue existing two-layer dirt wall and door") {
        if (input.at("questionResult").at("answer") != "靠河那段，留木门" ||
            input.at("task").at("goal") != "continue existing two-layer dirt wall and door")
            throw std::runtime_error("wall probe: clarification lost the original task");
        return call("observe","resurvey",{{"message",input.at("task").at("target").at("message")}});
    }
    const auto& event = input.at("events").back();
    if (event.at("sequence") == 2)
        return call("task_update","save",{{"goal","continue existing two-layer dirt wall and door"},
            {"detail","Find the player's original wall and leave the intended doorway"},{"next","player"}});
    if (event.at("sequence") == 3 && event.at("text") == "看我旁边的这块缺口")
        return call("task_update","point",{{"goal",input.at("task").at("goal")},
            {"next","player"},{"target",{{"message",3}}}});
    if (event.at("sequence") == 4 && event.at("text") == "先补刚才指的缺口，门洞留空") {
        if (gap_only && !ambiguous) return call("start_move","approach",{{"message",3}});
        return survey_original();
    }
    if (ambiguous && event.at("sequence") == 6 && event.at("text") == "我给你土了") {
        if (input.at("pendingApproval").at("skill") != "collect") throw std::runtime_error("gap probe: original collection was lost");
        return call("observe","stock",Json::object());
    }
    return internal("fixed event read");
}

// 用一条玩家原话触发门的识别及持续拆装，模型只在接纳和读回边界作决策。
// 步骤：绑定玩家指向 → 展开采集并识别现成门 → 提交拆装步骤 → 据实际安装发言。
dfy::model::Response door_sample(const dfy::model::Request& request) {
    using namespace dfy::model;
    using Json = nlohmann::json;
    const auto call = [&](std::string name, Json args) -> Response {
        const auto tool = std::ranges::find_if(request.tools,[&](const auto& def) { return def.name.ends_with("_"+name); });
        if (tool == request.tools.end()) throw std::runtime_error("door probe: missing "+name);
        return {{Role::assistant,"",{{name,tool->name,args.dump()}}},FinishReason::tool_calls};
    };
    const auto& last = request.messages.back();
    if (last.role == Role::user) {
        const auto input = Json::parse(last.content);
        if (input.at("trigger") == "skills.expanded" && !input.contains("activeMotion") &&
            std::ranges::any_of(request.tools,[](const auto& tool) { return tool.name.ends_with("_collect"); }))
            return call("observe",{{"around","self"},{"block","oak_door"}});
        if (input.at("trigger") == "motion.finished" && primary_receipt(input).at("skill") == "place" &&
            primary_receipt(input).at("result").at("placed") == true)
            return call("reply",{{"text","门已经装到你指的缺口了。"}});
        if (!input.at("events").empty() && input.at("events").back().at("sequence") == 2)
            return call("observe",{{"message",2}});
    } else if (last.role == Role::tool && last.tool_call_id == "observe") {
        const auto result = Json::parse(last.content.substr(last.content.find('{')));
        if (result.contains("anchor")) {
            if (result.at("playerFocus").at("choice") != "7,-60,-3:x")
                throw std::runtime_error("door probe: wrong opening for the original player view");
            return call("task_update",{{"goal","把现成门装到玩家指的缺口"},{"next","player"},
                {"target",{{"message",2},{"candidate","7,-60,-3:x"}}}});
        }
        if (result.at("doors").size() != 1 || result.at("doors").front().at("status") != "complete")
            throw std::runtime_error("door probe: existing door not verified");
        const auto& source = result.at("doors").front().at("lower");
        auto collect = source;
        collect["block"] = "oak_door";
        collect["count"] = 1;
        return call("queue_actions",{{"operation","append"},{"steps",Json::array({
            {{"skill","collect"},{"pickup","oak_door"},{"arguments",std::move(collect)}},
            {{"skill","place"},{"arguments",{{"block","oak_door"},{"x",7},{"y",-60},{"z",-3}}}}
        })}});
    } else if (last.role == Role::tool && last.tool_call_id == "task_update")
        return call("skills",{{"categories",{"dig_break"}}});
    return {{Role::assistant,R"({"internal":"waiting for verified door work"})"}};
}

// 在场景检验中限定从地面取材，依次验证越界候选、部分成功以及观察后的历史。
// 步骤：固定范围 → 尝试墙格并验证被拦截 → 读取部分采掘 → 重采现场后从任务读回。
dfy::model::Response materials_sample(const dfy::model::Request& request) {
    using namespace dfy::model;
    using Json = nlohmann::json;
    const auto call = [&](std::string_view skill, std::string id, Json args) -> Response {
        const auto tool = std::ranges::find_if(request.tools,[&](const auto& def) { return def.name.ends_with("_"+std::string(skill)); });
        if (tool == request.tools.end()) throw std::runtime_error("materials probe: tool missing");
        return {{Role::assistant,"",{{std::move(id),tool->name,args.dump()}}},FinishReason::tool_calls};
    };
    const auto& last = request.messages.back();
    if (last.role == Role::user) {
        const auto input = Json::parse(last.content);
        if (input.at("trigger") == "skills.expanded")
            return call("collect","wall",{{"block","dirt"},{"count",1},{"x",2},{"y",-59},{"z",-3}});
        if (input.at("trigger") == "motion.finished" && primary_receipt(input).at("skill") == "collect") {
            const auto& result = primary_receipt(input).at("result");
            if (result.at("status") != "blocked") throw std::runtime_error("materials probe: scope or partial receipt lost");
            if (result.at("changes").empty()) return call("collect","ground_collect",{{"block","dirt"},{"count",2}});
            if (result.at("changes").size() == 1) return call("observe","observe",Json::object());
            throw std::runtime_error("materials probe: changed unexpected cells");
        }
        if (!input.at("events").empty() && input.at("events").back().at("sequence") == 2)
            return call("task_update","ground",{{"goal","只从地面取泥土"},{"collection","ground"},{"next","player"}});
        if (!input.at("events").empty() && input.at("events").back().at("sequence") == 3) {
            const auto& task = input.at("task");
            if (task.at("lastAction").at("skill") != "observe" || task.at("worldChanges").size() != 1 ||
                task.at("worldChanges").front().at("before") != "dirt")
                throw std::runtime_error("materials probe: original changes lost after observation");
            return call("reply","reply",{{"text","我只挖了地上的一格泥土。"}});
        }
    } else if (last.role == Role::tool && last.tool_call_id == "ground")
        return call("skills","skills",{{"categories",{"dig_break"}}});
    return {{Role::assistant,R"({"internal":"materials observation complete"})"}};
}

// 用固定响应验收跟随交接、实际取材及四格放置，部分回执可继续原目标。
// 步骤：按场景处理原话 → 绑定观察候选 → 依次核对库存、实际变更和完工。
dfy::model::Response fill_four_sample(const dfy::model::Request& request, bool combined) {
    using namespace dfy::model;
    using Json = nlohmann::json;
    const auto use = [&](std::string id, std::string name, Json args) -> ToolCall {
        const auto tool = std::ranges::find_if(request.tools,[&](const auto& def) { return def.name.ends_with("_"+name); });
        if (tool == request.tools.end()) throw std::runtime_error("fill-four probe: missing "+name);
        return {id,tool->name,args.dump()};
    };
    const auto call = [&](std::string name, Json args) -> Response {
        return {{Role::assistant,"",{use(name,name,args)}},FinishReason::tool_calls};
    };
    const auto plan = [&](Json steps) { return call("queue_actions",{{"operation","plan"},{"steps",std::move(steps)}}); };
    const auto step = [](std::string id, std::string skill, Json arguments, Json after = Json::array()) {
        return Json{{"id",id},{"skill",skill},{"arguments",std::move(arguments)},{"after",std::move(after)}};
    };
    const Json cells = Json::array({{{"x",3},{"y",-60},{"z",-3}},{{"x",4},{"y",-60},{"z",-3}},
        {{"x",3},{"y",-59},{"z",-3}},{{"x",4},{"y",-59},{"z",-3}}});
    const auto& last = request.messages.back();
    if (last.role == Role::user) {
        const auto input = Json::parse(last.content);
        if (input.at("trigger") == "skills.expanded")
            return call("collect",{{"block","dirt"},{"count",1},{"x",3},{"y",-59},{"z",-3}});
        if (input.at("trigger") == "plan.finished") {
            const auto& graph = input.at("taskPlan");
            const auto& layers = input.at("planSettlements");
            if (layers.size() != 2 || graph.at("status") != "blocked") throw std::runtime_error("missing plan layers");
            const auto& first = layers.front().at("nodes");
            const auto& fill = layers.back().at("nodes").front().at("result");
            if (graph.at("supersedes") == 0) {
                if (first.size() != 1 || first.front().at("result").at("reasonCode") != "insufficient_materials" ||
                    fill.at("status") != "skipped")
                    throw std::runtime_error("insufficient logs did not block the dependent fill");
                return call("reply",{{"text","一根原木做不够五块木板，那四格我记着呢。"}});
            }
            const auto& collected = first.front().at("result");
            if (first.size() != 2 || first.back().at("result").at("status") != "done" ||
                collected.at("stock") != Json{{"item","dirt"},{"before",1},{"required",4},{"after",4}} ||
                collected.at("inventoryDelta").at("dirt") != 3 || fill.at("status") != "blocked" ||
                fill.at("changes").size() != 2 || fill.at("reasonCode") != "occupied")
                throw std::runtime_error("plan did not sequence gathered stock and the original partial fill");
            return call("reply",{{"text","下面两格补好了，上面有块石头挡着。"}});
        }
        if (input.at("trigger") == "motion.finished") {
            const auto& work = primary_receipt(input);
            const auto& settlement = input.at("workSettlement");
            if (settlement.at("invocation") != work.at("invocation") ||
                std::ranges::find(settlement.at("receipts"),work) == settlement.at("receipts").end())
                throw std::runtime_error("fill-four probe: final work identity lost");
            if (work.at("skill") == "collect" && input.contains("questionResult") &&
                (input.contains("pendingQuestion") || input.at("questionResult").at("status") != "resolved"))
                throw std::runtime_error("fill-four probe: observed player reference did not resolve the old question");
            const auto& result = work.at("result");
            if (work.value("pointedDig",false)) {
                if (result.at("changes").size() != 1 || result.at("inventoryDelta").at("dirt") != 1 ||
                    request.tools.size() != 1 || !request.tools.front().name.ends_with("_reply"))
                    throw std::runtime_error("fill-four probe: exact dig completion reopened exploration");
                return {{Role::assistant,R"({"internal":"exact dig settled"})"}};
            }
            if (result.contains("stock")) {
                if (result.at("stock") != Json{{"item","dirt"},{"before",combined ? 1 : 3},{"required",4},{"after",4}} ||
                    result.at("inventoryDelta").at("dirt") != (combined ? 3 : 1))
                    throw std::runtime_error("fill-four probe: ground gathering did not fill the actual deficit");
                return call("fill_target",{{"block","dirt"}});
            }
            if (work.at("skill") == "fill_target") {
                if (result.at("status") == "done" && result.at("cells").size() == 4)
                    return call("reply",{{"text","四格都补上了，另一处缺口留着。"}});
                if (combined && result.at("status") == "blocked" && result.at("changes").size() == 2 &&
                    result.at("cells").back().at("block") == "stone")
                    return call("reply",{{"text","下面两格补好了，上面有块石头挡着。"}});
                throw std::runtime_error("fill-four probe: unexpected partial receipt");
            }
        }
        const auto& events = input.at("events");
        const auto text = events.empty() ? std::string{} : events.back().value("text",std::string{});
        if (text == "你跟着我走")
            return {{Role::assistant,"",{use("follow-view","observe",Json::object())}},FinishReason::tool_calls};
        if (text == "你还跟着吗" && input.contains("following")) return {{Role::assistant,"嗯，跟着呢。"}};
        if (text == "把这个草方块挖掉" && input.contains("following"))
            return {{Role::assistant,"",{use("find-hit","observe",{{"message",events.back().at("sequence")},{"focus","hit"}})}},
                FinishReason::tool_calls};
        if (text == "你听得见吗") {
            if (!input.contains("pendingQuestion")) throw std::runtime_error("fill-four probe: unrelated chat cleared question");
            return call("reply",{{"text","听得见。"}});
        }
        if (text == "我面前的这个" && input.contains("following"))
            return call("observe",{{"message",events.back().at("sequence")},{"focus","hit"}});
        if (text == "先别跟了") return call("stop_motion",{{"work",input.at("following").at("work")}});
        if (text.starts_with("石头挪开了") && input.at("task").at("target").at("message") == 8)
            return call("fill_target",{{"block","dirt"}});
        if (text == "你站到墙上，先换个目标。")
            return call("task_update",{{"goal","站到墙上"},{"next","player"},{"collection","visible"},{"target",Json::object()}});
        if (text == "试一下普通采掘入口。")
            return call("skills",{{"categories",{"dig_break"}}});
        if (text == "验收错误依赖。") {
            const auto cycle = Json::array({step("a","observe",Json::object(),{"b"}),step("b","observe",Json::object(),{"a"})});
            const auto missing = Json::array({step("a","observe",Json::object(),{"b"})});
            const auto incompatible = Json::array({step("move","move",{{"place","player"}},{"collect"})});
            return {{Role::assistant,"",{use("cycle","queue_actions",{{"operation","plan"},{"steps",cycle}}),
                use("missing","queue_actions",{{"operation","plan"},{"steps",missing}}),
                use("incompatible","queue_actions",{{"operation","append"},{"steps",incompatible}})}},FinishReason::tool_calls};
        }
        if (text == "那你在地上挖四个泥土，把墙补了") {
            if (input.at("task").at("target").at("message") != 8 || input.at("taskPlan").at("status") != "blocked")
                throw std::runtime_error("resumed plan lost its original opening");
            return {{Role::assistant,"",{use("check-stock","observe",Json::object())}},FinishReason::tool_calls};
        }
        if (text.starts_with("从地上取够泥土") || text.starts_with("把原木做成五个木板"))
            return call("observe",{{"sources",{"message","self"}},{"message",events.back().at("sequence")},{"radius",5},{"verify",cells},
                {"samples",{{"count",2},{"intervalMs",100}}}});
    } else if (last.role == Role::tool) {
        const auto result = Json::parse(last.content.substr(last.content.find('{')));
        if (last.tool_call_id == "follow-view") {
            if (!result.contains("player") || !result.contains("inventory")) throw std::runtime_error("missing follow facts");
            return {{Role::assistant,"",{use("follow","follow",{{"distance",2}}),
                use("follow-reply","reply",{{"text","好，我跟着你。"}})}},FinishReason::tool_calls};
        }
        if (last.tool_call_id == "follow-reply") throw std::runtime_error("redundant decision after follow and reply");
        if (last.tool_call_id == "ask_player" && result.at("status") == "recorded")
            throw std::runtime_error("model ran before a new fact resolved the recorded question");
        if (last.tool_call_id == "queue_actions" && result.at("status") == "queued")
            throw std::runtime_error("model ran without a layer completion or a new player event");
        if (last.tool_call_id == "check-stock") {
            if (result.at("inventory").at("dirt") != 1) throw std::runtime_error("ground work lost the previous pickup");
            return plan(Json::array({step("stock","gather_ground",{{"item","dirt"},{"requiredStock",4}}),
                step("look","observe",Json::object()),step("fill","fill_target",{{"block","dirt"}},{"stock","look"})}));
        }
        if (last.tool_call_id == "find-hit") {
            if (!result.at("playerFocus").at("hit").is_null() || result.contains("survey"))
                throw std::runtime_error("expected short, unresolved hit observation");
            const auto message = result.at("anchor").at("message");
            return {{Role::assistant,"",{use("extra-view","observe",{{"sources",{"message","self","player"}},{"message",message},
                {"radius",2},{"focus","hit"}})}},FinishReason::tool_calls};
        }
        if (last.tool_call_id == "extra-view") {
            if (!result.at("observation").at("samples").front().at("views").front().at("playerFocus").at("hit").is_null())
                throw std::runtime_error("ambiguous request unexpectedly identified a block");
            const auto message = result.at("observation").at("samples").front().at("views").front().at("message");
            return {{Role::assistant,"",{
                use("third-hit","observe",{{"message",message},{"focus","hit"}}),
                use("guessed-dig","dig_block",{{"message",message},{"block","grass_block"},{"x",2},{"y",-61},{"z",0}}),
                use("ask_player","ask_player",{{"question","你指哪一格？"}})}},FinishReason::tool_calls};
        }
        if ((last.tool_call_id == "dig_block" || last.tool_call_id == "gather_ground" || last.tool_call_id == "fill_target") &&
            (result.at("status") == "accepted" || result.at("status") == "queued"))
            throw std::runtime_error("fill-four probe: model called without a new game receipt");
        if (last.tool_call_id == "observe") {
            if (result.contains("playerFocus")) {
                const auto& hit = result.at("playerFocus").at("hit");
                if (hit != Json{{"x",2},{"y",-61},{"z",0},{"block","grass_block"}})
                    throw std::runtime_error("fill-four probe: player line of sight did not hit the exact block");
                auto args = hit;
                const auto input = Json::parse(std::ranges::find_if(request.messages,[](const auto& msg) {
                    return msg.role == Role::user;
                })->content);
                args["resolves"] = input.at("pendingQuestion").at("item");
                args["message"] = result.at("anchor").at("message");
                return call("dig_block",args);
            }
            const auto& samples = result.at("observation").at("samples");
            const auto& sample = samples.back();
            if (samples.size() != 2 || sample.at("sampledAtMs") <= samples.front().at("sampledAtMs") ||
                sample.at("verified").size() != 4 || !std::ranges::all_of(sample.at("verified"),[](const auto& cell) {
                return cell.at("block") == "air";
            })) throw std::runtime_error("fill-four probe: missing real empty cells");
            const auto& origin = sample.at("views").front();
            if (combined && sample.at("views").back().at("actor").at("inventory").at("oak_log") != 1)
                throw std::runtime_error("requested craft has no observed log");
            const auto& gaps = origin.at("playerFocus").at("candidates");
            const auto gap = std::ranges::find_if(gaps,[](const auto& candidate) {
                return candidate.at("id") == "3,-60,-3:x" && candidate.at("width") == 2 && candidate.at("height") == 2;
            });
            const auto door = std::ranges::find_if(gaps,[](const auto& candidate) {
                return candidate.at("id") == "7,-60,-3:x" && candidate.at("width") == 1 && candidate.at("height") == 2;
            });
            if (gap == gaps.end() || door == gaps.end()) throw std::runtime_error("fill-four probe: two openings were not distinct");
            return call("task_update",{{"goal","补上指定的四个墙格"},{"next","player"},{"collection","ground"},
                {"target",{{"message",origin.at("message")},{"candidate",gap->at("id")}}}});
        }
        if (last.tool_call_id == "task_update") {
            if (result.at("task").at("goal") == "站到墙上")
                return call("dig_block",{{"block","dirt"},{"x",3},{"y",-59},{"z",-3}});
            if (result.at("task").at("target").at("cells") != cells) throw std::runtime_error("fill-four probe: gap bound wrong cells");
            if (combined) return plan(Json::array({step("planks","craft",{{"item","oak_planks"},{"count",5}}),
                step("fill","fill_target",{{"block","oak_planks"}},{"planks"})}));
            return call("gather_ground",{{"item","dirt"},{"requiredStock",4}});
        }
    }
    return {{Role::assistant,R"({"internal":"game work continues"})"}};
}

// 用隔离服的观察和合成回执决定何时向玩家报告产出。
// 步骤：按原话采样 → 核对已加载方块和原料 → 提交合成并等待实际库存变化。
dfy::model::Response craft_sample(const dfy::model::Request& request) {
    using namespace dfy::model;
    using Json = nlohmann::json;
    const auto call = [&](std::string_view name, const Json& args) -> Response {
        const auto tool = std::ranges::find_if(request.tools,[&](const auto& def) { return def.name.ends_with(name); });
        if (tool == request.tools.end()) throw std::runtime_error("craft probe: required tool missing");
        return {{Role::assistant,"",{{std::string(name),tool->name,args.dump()}}},FinishReason::tool_calls};
    };
    const auto& last = request.messages.back();
    if (last.role == Role::tool && last.tool_call_id == "_observe") {
        const auto result = Json::parse(last.content.substr(last.content.find('{')));
        const auto& sample = result.at("observation").at("samples").front();
        if (sample.at("verified").front().at("block") != "dirt" || sample.at("views").size() != 3 ||
            sample.at("views").front().at("actor").at("inventory").at("oak_log") != 1)
            throw std::runtime_error("craft probe: expected actual block and input stock");
        const Json node{{"id","planks"},{"skill","craft"},{"arguments",{{"item","oak_planks"},{"count",4},{"ingredient","oak_log"}}}};
        return call("_queue_actions",{{"operation","plan"},{"steps",Json::array({node})}});
    }
    if (last.role == Role::user) {
        const auto input = Json::parse(last.content);
        if (input.at("trigger") == "plan.finished") {
            const auto& result = input.at("planSettlements").at(0).at("nodes").at(0).at("result");
            if (result.at("status") != "done" || result.at("inventoryDelta") != Json{{"oak_log",-1},{"oak_planks",4}} ||
                result.at("craft").at("remaining") != 0)
                throw std::runtime_error("craft probe: outcome was not verified");
            return {{Role::assistant,"原木已经做成四块橡木木板。"}};
        }
        if (!input.at("events").empty() && input.at("events").back().value("text",std::string{}) == "看看你旁边，把原木做成四块木板。")
            return call("_observe",{{"mode","scene"},{"sources",{"self","player","message"}},{"message",input.at("events").back().at("sequence")},
                {"verify",Json::array({{{"x",3},{"y",-60},{"z",0}}})}});
    }
    return {{Role::assistant,R"({"internal":"waiting for craft probe"})"}};
}

// 用固定响应调度两次渡水，聊天和停下只使用当前跟随工作的实际身份。
dfy::model::Response water_sample(const dfy::model::Request& request) {
    using namespace dfy::model;
    using Json = nlohmann::json;
    const auto call = [&](std::string name, Json args) -> Response {
        const auto tool = std::ranges::find_if(request.tools,[&](const auto& def) { return def.name.ends_with("_"+name); });
        if (tool == request.tools.end()) throw std::runtime_error("water probe: missing "+name);
        return {{Role::assistant,"",{{name,tool->name,args.dump()}}},FinishReason::tool_calls};
    };
    const auto& last = request.messages.back();
    if (last.role == Role::user) {
        const auto input = Json::parse(last.content);
        if (!input.at("events").empty()) {
            const auto text = input.at("events").back().value("text",std::string{});
            if (text == "跟我渡水") return call("follow",{{"distance",2}});
            if (text == "水里还跟着吗" && input.at("activeMotion").at("skill") == "follow")
                return call("reply",{{"text","我还跟着你。"}});
            if (text == "这里停下") return call("stop_motion",{{"work",input.at("activeMotion").at("work")}});
            if (text == "回到西岸" || text == "去封住的地方") {
                const auto target = text == "回到西岸" ? Json{{"x",-38},{"y",-60},{"z",-25}} :
                    Json{{"x",-30},{"y",-59},{"z",-15}};
                return call("queue_actions",{{"operation","append"},{"steps",Json::array({
                    {{"skill","move"},{"arguments",target}}
                })}});
            }
        }
    }
    return {{Role::assistant,R"({"internal":"water observation complete"})"}};
}
}

// 保持真人体验的正式宿主运行，向本地日志发布进展和计划板并完整收尾。
// 步骤：绑定原生包和独立数据目录 → 推进玩家委托与回执 → 按退出请求回收插件。
int wmain(int argc, wchar_t** argv) {
    using namespace dfy;
    try {
        const auto mode = argc == 4 ? std::wstring_view(argv[3]) : std::wstring_view{};
        const bool manual = mode == L"--ask-main-manual-probe";
        const std::string_view ask = mode == L"--ask-direct-probe" ? "direct" :
            manual || mode == L"--ask-main-probe" ? "main" : mode == L"--gap-ambiguous-probe" ? "local" : "";
        const bool gap = mode == L"--gap-probe" || mode == L"--gap-ambiguous-probe";
        const bool wall = !ask.empty() || mode == L"--wall-probe" || gap;
        const bool follow = mode == L"--follow-probe";
        const bool door = mode == L"--door-probe";
        const bool materials = mode == L"--gap-materials-probe";
        const bool combined = mode == L"--follow-dig-fill-probe";
        const bool fill_four = combined || mode == L"--fill-four-probe";
        const bool water = mode == L"--water-probe";
        const bool resupply = mode == L"--layered-resupply-probe";
        const bool layered = resupply || mode == L"--layered-follow-probe";
        const bool craft = mode == L"--craft-probe";
        const bool partner = wall || follow || door || materials || fill_four || water || layered || craft || mode == L"--partner-probe";
        if (argc != 3 && !partner) throw std::invalid_argument("package and playtest directory required");
        const std::filesystem::path root = argv[2];
        app::ApplicationRunConfig run;
        if (partner) run.trace_directory = root;
        run.wake_for_plugin_mail = !manual;
        model::Config config;
        if (follow) {
            config.provider = "deepseek";
            config.model = "fixed-follow";
            config.thinking = true;
        }
        app::Application host(std::move(config), root / "packages", root / "user", model::Backend{
            [partner,wall,follow,door,materials,fill_four,combined,water,layered,resupply,craft,ask,gap,root](const auto&, const auto& request) {
                if (craft) return craft_sample(request);
                if (layered) return layered_follow_sample(request,resupply);
                if (water) return water_sample(request);
                if (fill_four) return fill_four_sample(request,combined);
                if (materials) return materials_sample(request);
                if (door) return door_sample(request);
                if (wall) return wall_sample(request,root,ask,gap);
                if (follow) {
                    auto response = follow_sample(request);
                    response.message.reasoning_content = "fixed provider continuation";
                    return response;
                }
                return partner ? partner_sample(request,root) : sample_turn(request);
            }},
            std::move(run));
        app::run_companion_host(host, argv[1], root, partner ? app::CompanionMode::partner : app::CompanionMode::scenario);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
