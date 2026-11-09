#include <dafeiyu/plugin.hpp>
class External : public dfy::sdk::Plugin {
public:
    static constexpr auto id = "external.example";
    static constexpr auto version = "0.2.0";
    External(dfy::sdk::Host host, const dfy::sdk::Json&) : Plugin(host) { host.provide("action.double"); }
    void on_event(const dfy::sdk::Json& event) {
        if (event.at("type") != "call") return;
        const dfy::sdk::Call call(event);
        if (call.method != "execute") throw std::runtime_error("METHOD_UNSUPPORTED");
        const int value = call.input.at("value");
        if (value < -100 || value > 100) throw std::runtime_error("VALUE_RANGE");
        host.complete(call.id, dfy::sdk::outcome({{"value", value * 2}}));
    }
};
DFY_PLUGIN(External)
