#pragma once
#include "operations.hpp"
namespace dfy {
class ContextAssembler {
    StateStore& store_;
    PluginRuntime& plugins_;
    CallRouter& calls_;
    PolicyGate& policy_;
    int timeout_;
    size_t budget_;
public:
    ContextAssembler(StateStore& store, PluginRuntime& plugins, CallRouter& calls, PolicyGate& policy, const Profile& profile);
    void assemble(const Json& run, const Json& proposal, std::function<void(const Completion&)> done);
};
}
