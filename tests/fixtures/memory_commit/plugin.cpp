#include <dafeiyu/plugin.hpp>
#include <map>
using namespace dfy::sdk;

class Remember : public Plugin {
    std::map<uint64_t, Json> inputs_;

public:
    static constexpr auto id = "dafeiyu.action.remember";
    static constexpr auto version = "0.5.0";

    // 登记通过嵌套记忆服务完成写入的测试动作。
    Remember(Host host, const Json&) : Plugin(host) { host.provide("action.remember"); }

    // 将记忆动作拆为读取快照和提交变更，并支持结果查询。
    // 步骤：接收动作并保存输入 → 请求人格快照 → 在回复中按版本提交变更 → 回传变更结果或查询回执。
    void on_event(const Json& event) {
        if (event.at("type") == "reply") {
            const auto call = event.at("call_id").get<uint64_t>();
            if (!event.at("error").is_null()) {
                inputs_.erase(call);
                host.fail(call, event.at("error"));
                return;
            }

            if (event.at("key") == "snapshot") {
                const auto input = inputs_.at(call);
                inputs_.erase(call);
                host.request(call, "commit", "memory.changes", "commit_change",
                    {{"expected_revision", event.at("result").at("revision")}, {"persona", input.at("persona")}});
            } else
                host.complete(call, event.at("result"));
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
        else if (call.method == "execute") {
            inputs_[call.id] = call.input;
            host.request(call.id, "snapshot", "memory", "snapshot", Json::object());
        } else if (call.method == "query_operation")
            host.request(call.id, "query", "memory.changes", "query_change",
                {{"id", call.input.at("id").get<std::string>() + ":commit"}});
        else
            throw std::runtime_error("METHOD_UNSUPPORTED");
    }
};
DFY_PLUGIN(Remember)
