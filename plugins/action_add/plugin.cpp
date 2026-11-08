#include <dafeiyu/plugin.hpp>
using namespace dfy::sdk;
class AddAction {
public:
    static constexpr auto id = "dafeiyu.action.add";
    static constexpr auto version = "0.1.0";
    AddAction(Host host, const Json&) { host.provide("action.add"); }
    void start() {}
    void stop() {}
    void invoke(const Call& call, const Output& output) {
        if (call.method == "readiness") { output.emit({{"available", true}, {"reason", nullptr}}); return; }
        if (call.method != "execute") throw std::runtime_error("METHOD_UNSUPPORTED");
        if (!call.input.is_object() || call.input.size() != 2 || !call.input.at("a").is_number_integer() || !call.input.at("b").is_number_integer())
            throw std::runtime_error("ADD_ARGUMENTS_INVALID");
        const auto a = call.input.at("a").get<int64_t>(), b = call.input.at("b").get<int64_t>();
        if (a < -1000000 || a > 1000000 || b < -1000000 || b > 1000000) throw std::runtime_error("ADD_ARGUMENT_RANGE");
        call.checkpoint();
        output.emit({{"status", "succeeded"}, {"effects", "none"}, {"coverage", "complete"}, {"data", {{"sum", a + b}}},
            {"sources", Json::array()}, {"receipts", Json::array()}});
    }
};
DFY_PLUGIN(AddAction)
