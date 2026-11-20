#pragma once
#include <dafeiyu/async_model.hpp>
#include <dafeiyu/run_identity.hpp>
#include <string_view>

namespace dfy::agent {
enum class ModelCapturePhase { prepared, submitted, exited, rewritten };

// 仅在宿主同步回调期间借用模型内容。请求配置只提供模型和供应商标签。
struct ModelCapture {
    ModelCapturePhase phase;
    AttemptId attempt;
    std::string_view provider, model_name;
    const model::Request* input = nullptr;
    const model::Response* output = nullptr;
    const model::CallDiagnostics* call = nullptr;
    runtime::OperationState result = runtime::OperationState::pending;
};

using ModelCaptureSink = std::function<void(const ModelCapture&)>;
}
