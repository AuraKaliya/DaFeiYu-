#include "companion_host.hpp"
#include "application.hpp"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>

namespace {
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
            if (!has("move")) return call("skills",{{"categories",{"movement"}}});
            return call("move", {{"x",seq == 4 ? 32 : 1},{"y",-60},{"z",-4}});
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
    if (last.role == Role::tool && last.tool_call_id == "task_update") {
        const auto result = Json::parse(last.content.substr(last.content.find('{')));
        const auto& task = result.at("task");
        if (task.at("revision") == 1) return call("follow",Json::object());
        if (task.at("revision") != 2 || task.at("next") != "done") throw std::runtime_error("follow probe: revision did not advance");
    }
    if (last.role == Role::user) {
        const auto input = Json::parse(last.content);
        if (input.at("trigger") == "motion.finished") {
            const auto& work = input.at("finishedWork");
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
            if (text == "到哪了") return call("reply",{{"text","我还在跟着你。"}});
            if (text == "继续边走边聊") return {{Role::assistant,"我继续跟着你。"}};
            if (text == "先别跟了") return call("stop_motion",{{"work",motion.at("work")}});
        }
    }
    return {{Role::assistant,R"({"internal":"processed"})"}};
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
// 步骤：保存原分工及指向 → 对照旧位置和后来位置 → 拒绝错误门洞并补一格 → 等待游戏玩家确认门。
dfy::model::Response wall_sample(const dfy::model::Request& request, const std::filesystem::path& root, std::string_view ask_path) {
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
    // 收到放置读回后才开始门洞提议，立即退出和在途工作读回使用同一任务变更。
    const auto plan_door = [&] {
        return call("task_update","door-plan",{{"goal","put the agreed door in its opening"},
            {"next","player"},{"target",Json::object()}});
    };
    if (last.role == Role::tool) {
        const auto result = Json::parse(last.content.substr(last.content.find('{')));
        if (last.tool_call_id == "save" || last.tool_call_id == "point" || last.tool_call_id == "directory" ||
            last.tool_call_id == "question") return internal("fixed round done");
        if (last.tool_call_id == "survey" || last.tool_call_id == "resurvey") {
            const auto& survey = result.at("survey");
            if (survey_block(survey,2,-59,-3) != "dirt" || survey_block(survey,3,-59,-3) != "." ||
                survey_block(survey,3,-60,-3) != "dirt" || survey_block(survey,7,-59,-3) != "." ||
                result.at("anchor").at("message") != 3 || result.at("anchor").at("position").at("x") >= 4 ||
                result.at("player").at("x") < 7)
                throw std::runtime_error("wall probe: survey lost the original gap or the separate live player position");
            if (!ask_path.empty() && last.tool_call_id == "survey")
                return call("ask_player","question",{{"question","你指靠河的那段，还是靠木屋的那段？"},{"path",ask_path}});
            const auto& focus = result.at("playerFocus");
            if (focus.at("choice") != "3,-59,-3:x" || !std::ranges::any_of(focus.at("candidates"),[](const auto& gap) {
                return gap.at("id") == "7,-60,-3:x" && gap.at("height") == 2;
            })) throw std::runtime_error("wall probe: original player view did not identify gap A with a separate gap B");
            return call("task_update","bind",{{"goal","continue existing two-layer dirt wall and door"},{"next","player"},
                {"target",{{"message",3},{"candidate",focus.at("choice")}}}});
        }
        if (last.tool_call_id == "wrong-gap" && result.at("status") == "blocked" && result.at("placementAttempted") == false)
            return call("place","wall",{{"block","dirt"},{"x",3},{"y",-59},{"z",-3}});
        if (last.tool_call_id == "wall" && result.at("status") == "done" && result.at("placed") == true)
            return plan_door();
        if (last.tool_call_id == "door-plan" && result.at("status") == "updated")
            return call("place","door",{{"block","oak_door"},{"x",7},{"y",-60},{"z",-3},{"approval","game"}});
        return internal("fixed action ended");
    }
    const auto input = Json::parse(last.content);
    record_input(root,input);
    if (request.messages.size() != 2) throw std::runtime_error("wall probe: older raw context was sent again");
    if (input.at("trigger") == "motion.finished") {
        const auto& finished = input.at("finishedWork");
        if (finished.at("skill") == "place" && finished.at("result").value("placed",false)) return plan_door();
        return internal("fixed action ended");
    }
    if (input.at("trigger") == "skills.expanded") {
        const auto& task = input.at("task");
        if (!task.at("target").contains("cells")) {
            if (std::ranges::any_of(request.tools,[](const auto& tool) { return tool.name.ends_with("_place"); }))
                throw std::runtime_error("wall probe: place granted before selecting a target");
            return internal("原位置已知，等玩家指定目标");
        }
        const auto& observation = task.at("lastAction").at("result");
        if (task.at("goal") != "continue existing two-layer dirt wall and door" || task.at("target").at("message") != 3 ||
            task.at("detail") != "Find the player's original wall and leave the intended doorway" ||
            !observation.value("surveyAvailable",false) || observation.contains("survey") || !observation.contains("sampledAtMs") ||
            !input.at("events").empty() || input.at("recentDialogue").front().at("order") != 2)
            throw std::runtime_error("wall probe: expansion lost prior goal or environment");
        return call("place","wrong-gap",{{"block","dirt"},{"x",7},{"y",-60},{"z",-3}});
    }
    if (input.at("trigger") == "question.result") {
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
    if (event.at("sequence") == 4 && event.at("text") == "先补刚才指的缺口，门洞留空")
        return call("observe","survey",{{"message",input.at("task").at("target").at("message")}});
    return internal("fixed event read");
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
            manual || mode == L"--ask-main-probe" ? "main" : "";
        const bool wall = !ask.empty() || mode == L"--wall-probe";
        const bool follow = mode == L"--follow-probe";
        const bool partner = wall || follow || mode == L"--partner-probe";
        if (argc != 3 && !partner) throw std::invalid_argument("package and playtest directory required");
        const std::filesystem::path root = argv[2];
        app::ApplicationRunConfig run;
        if (partner) run.trace_directory = root;
        run.wake_for_plugin_mail = !manual;
        app::Application host({}, root / "packages", root / "user", model::Backend{
            [partner,wall,follow,ask,root](const auto&, const auto& request) {
                if (wall) return wall_sample(request,root,ask);
                if (follow) return follow_sample(request);
                return partner ? partner_sample(request,root) : sample_turn(request);
            }},
            std::move(run));
        app::run_companion_host(host, argv[1], root, partner ? app::CompanionMode::partner : app::CompanionMode::scenario);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
