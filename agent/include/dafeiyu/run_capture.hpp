#pragma once
#include <dafeiyu/async_model.hpp>
#include <dafeiyu/run_identity.hpp>
#include <string_view>

namespace dfy::agent {
enum class ModelCapturePhase { prepared, submitted, exited, rewritten };

// 仅在宿主同步回调期间借用模型内容。请求配置只提供模型、供应商和思考模式。
struct ModelCapture {
    ModelCapturePhase phase;
    AttemptId attempt;
    std::string_view provider, model_name;
    const model::Request* input = nullptr;
    const model::Response* output = nullptr;
    const model::CallDiagnostics* call = nullptr;
    runtime::OperationState result = runtime::OperationState::pending;
    bool thinking = false;
    std::string_view persona_profile, prompt_template;
};

using ModelCaptureSink = std::function<void(const ModelCapture&)>;
enum class ToolCapturePhase { candidate, admitted, exited };
struct BatchToolSnapshot;
class ToolRequest;
// 工具阶段记录只借用原候选、命令及已接纳工作，拒绝时没有工作引用。
struct ToolCapture {
    ToolCapturePhase phase;
    const BatchToolSnapshot& candidate;
    const model::ToolCall& call;
    const ToolRequest* request;
};
using ToolCaptureSink = std::function<void(const ToolCapture&)>;
class TurnRequest;
using TurnExitSink = std::function<void(const TurnRequest&)>;
struct RunCaptureSinks {
    ModelCaptureSink model;
    TurnExitSink exit;
    ToolCaptureSink tool;
};
}
