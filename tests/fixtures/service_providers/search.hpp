#pragma once
#include <dafeiyu/plugin.hpp>

namespace dfy::examples {
class Search : public sdk::Plugin {
    sdk::Json config_;

public:
    // 保存提供者配置，并登记该测试提供者的搜索服务。
    Search(sdk::Host host, sdk::Json config) : Plugin(host), config_(std::move(config)) {
        host.provide(config_.at("service").get<std::string>());
    }

    // 返回带提供者标签的搜索条目，用于验证服务绑定替换。
    void on_event(const sdk::Json& event) {
        if (event.at("type") != "call")
            return;
        const sdk::Call call(event);

        if (call.method != "query")
            throw std::runtime_error("METHOD_UNSUPPORTED");

        const auto label = config_.at("label").get<std::string>();
        host.complete(
            call.id, {{"items", sdk::Json::array({{{"title", label + ": " + call.input.at("query").get<std::string>()},
                                    {"source", "fixture"}}})},
                         {"provider", label}});
    }
};
}
