#pragma once
#include <dafeiyu/plugin.hpp>
namespace dfy::examples {
class Search : public sdk::Plugin {
    sdk::Json config_;
public:
    Search(sdk::Host host, sdk::Json config) : Plugin(host), config_(std::move(config)) { host.provide(config_.at("service").get<std::string>()); }
    void on_event(const sdk::Json& event) {
        if (event.at("type") != "call") return;
        const sdk::Call call(event);
        if (call.method != "query") throw std::runtime_error("METHOD_UNSUPPORTED");
        const auto label = config_.at("label").get<std::string>();
        host.complete(call.id, {{"items", sdk::Json::array({{{"title", label + ": " + call.input.at("query").get<std::string>()}, {"source", "fixture"}}})}, {"provider", label}});
    }
};
}
