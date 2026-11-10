#include <dafeiyu/plugin.hpp>
using namespace dfy::sdk;

class BehaviorAction : public Plugin {
public:
    static constexpr auto id = "dafeiyu.action.behavior";
    static constexpr auto version = "0.5.0";

    // 登记通过宿主命令修改行为模式的测试动作。
    BehaviorAction(Host host, const Json&) : Plugin(host) { host.provide("action.behavior"); }

    // 将动作执行转换为受控行为命令，并查询或消费命令回执。
    // 步骤：区分命令回复与动作调用 → 转换标准动作输入 → 提交变更或查询回执 → 回传确认、未知或失败结果。
    void on_event(const Json& event) {
        if (event.at("type") == "reply") {
            if (!event.at("error").is_null())
                host.fail(event.at("call_id"), event.at("error"));
            else if (event.at("result").is_null())
                host.complete(event.at("call_id"), outcome(Json::object(), "unknown", "unknown"));
            else
                host.complete(event.at("call_id"), outcome(event.at("result"), "confirmed"));
            return;
        }

        if (event.at("type") != "call")
            return;
        Call call(event);
        if (call.method == "execute" || call.method == "readiness" || call.method == "query_operation") {
            const ActionInvocation action(call);
            call.input = action.arguments;
            if (call.method == "query_operation")
                call.input["id"] = action.operation_id;
        }

        if (call.method == "readiness")
            host.complete(call.id, {{"available", true}});
        else if (call.method == "execute")
            host.command(call.id, "change", "behavior.set", call.input);
        else if (call.method == "query_operation")
            host.command_receipt(call.id, "change");
        else
            throw std::runtime_error("METHOD_UNSUPPORTED");
    }
};
DFY_PLUGIN(BehaviorAction)
