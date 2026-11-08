#include "../../plugins/channel_simulator/simulator.hpp"
#include <cstdlib>
class CrashChannel : public dfy::examples::Simulator {
public:
    static constexpr auto id = "test.crash-channel";
    using Simulator::Simulator;
    void invoke(const dfy::sdk::Call& call, const dfy::sdk::Output& output) {
        Simulator::invoke(call, output);
        // Durable platform receipt exists, but the host has not committed it yet.
        if (call.method == "deliver") std::_Exit(73);
    }
};
DFY_PLUGIN(CrashChannel)
