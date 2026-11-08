#include <dafeiyu/plugin.hpp>
class ExternalPlugin {
public:
    static constexpr auto id = "external.example";
    static constexpr auto version = "0.1.0";
    ExternalPlugin(dfy::sdk::Host host, const dfy::sdk::Json&) { host.provide("action.double"); }
    void start() {}
    void stop() {}
    void invoke(const dfy::sdk::Call& call, const dfy::sdk::Output& output) {
        if (call.method == "readiness") { output.emit({{"available",true}}); return; }
        if (call.method != "execute") throw std::runtime_error("METHOD_UNSUPPORTED");
        const auto value = call.input.at("value").get<int>();
        if (value < -100 || value > 100) throw std::runtime_error("VALUE_RANGE");
        output.emit({{"status","succeeded"},{"effects","none"},{"coverage","complete"},
            {"data",{{"value",value*2}}},{"sources",dfy::sdk::Json::array()},{"receipts",dfy::sdk::Json::array()}});
    }
};
DFY_PLUGIN(ExternalPlugin)
