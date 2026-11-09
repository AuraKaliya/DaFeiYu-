#pragma once
#include "operations.hpp"
namespace dfy {
class JobCoordinator {
    StateStore& store_;
    StateWriter writer_;
    PluginRuntime& plugins_;
    CallRouter& calls_;
    OperationCoordinator& operations_;
    int timeout_, max_jobs_;
public:
    JobCoordinator(StateStore& store, PluginRuntime& plugins, CallRouter& calls, OperationCoordinator& operations, const Profile& profile);
    void register_job(const PluginMessage& message);
    void job_result(const PluginMessage& message);
    void cancel_jobs(const std::string& run);
    void recover();
};
}
