#include <dafeiyu/plugin.hpp>
using namespace dfy::sdk;

class Probe : public Plugin {
    Json config_;

public:
    static constexpr auto id = "test.probe";
    static constexpr auto version = "0.5.0";

    // 保存探针配置并登记通用协议测试服务。
    Probe(Host host, Json config) : Plugin(host), config_(std::move(config)) { host.provide("fixture.probe"); }

    // 根据配置模拟插件启动失败。
    void start() {
        if (config_.value("fail_start", false))
            throw std::runtime_error("FIXTURE_START_FAILURE");
    }

    // 模拟嵌套越权、伪造调用身份或重复终态消息。
    // 步骤：转发依赖回复 → 响应就绪查询 → 按探针模式构造请求 → 回传测试结果或重复完成消息。
    void on_event(const Json& event) {
        if (event.at("type") == "reply") {
            if (!event.at("error").is_null())
                host.fail(event.at("call_id"), event.at("error"));
            else
                host.complete(event.at("call_id"), outcome(event.at("result")));
            return;
        }

        if (event.at("type") != "call")
            return;
        const Call call(event);
        if (call.method == "readiness") {
            host.complete(call.id, {{"available", true}});
            return;
        }

        const auto mode = config_.value("mode", std::string("duplicate"));
        if (mode == "nested")
            host.request(call.id, "write", "memory.changes", "commit_change",
                {{"expected_revision", 1}, {"persona", "unauthorized"}});
        else if (mode == "forge") {
            const auto status = host.post({{"type", "request_service"}, {"call_id", call.id + 1000000},
                {"key", "forge"}, {"binding", "memory"}, {"method", "snapshot"}, {"input", Json::object()}});
            host.complete(call.id, outcome({{"rejected", status != DFY_OK}}));
        } else {
            const Json message{{"type", "complete"}, {"call_id", call.id}, {"result", outcome({{"value", 1}})}};
            host.send(message);
            (void)host.post(message);
        }
    }
};
DFY_PLUGIN(Probe)
