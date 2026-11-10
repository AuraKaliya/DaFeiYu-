#pragma once
#include "operations.hpp"

namespace dfy {
class ContextAssembler {
    StateStore& store_;
    PluginRuntime& plugins_;
    CallRouter& calls_;
    PolicyGate& policy_;
    ScopeManager& scopes_;
    int timeout_;
    size_t budget_;

public:
    // 连接上下文采集依赖，并读取调用超时和上下文字节预算。
    ContextAssembler(StateStore& store, PluginRuntime& plugins, CallRouter& calls, PolicyGate& policy,
        ScopeManager& scopes, const Profile& profile);
    // 异步汇总运行任务所需的记忆、人格、上下文及可见动作。
    void assemble(const Json& run, const Json& proposal, std::function<void(const Completion&)> done);
};
}
