#include <dafeiyu/plugin.hpp>
class FailStart {
public:
    static constexpr auto id = "zz.test.fail";
    static constexpr auto version = "0.1.0";
    FailStart(dfy::sdk::Host host, const dfy::sdk::Json&) { host.provide("test.fail"); }
    void start() { throw std::runtime_error("EXPECTED_START_FAILURE"); }
    void stop() {}
    void invoke(const dfy::sdk::Call&, const dfy::sdk::Output&) {}
};
DFY_PLUGIN(FailStart)
