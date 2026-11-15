#include <dafeiyu/async_model.hpp>
#include <atomic>
#include <stdexcept>

namespace dfy::model {
using namespace runtime;
// 后台单次发布的时间字段通过标志同步；发布后不再改写，宿主不读取未发布字段。
struct CallProgress {
    CallDiagnostics::TimePoint submitted, started, finished;
    CallOutcome outcome = CallOutcome::pending;
    std::atomic<bool> has_started{false}, has_finished{false};
    // 在进入供应商调用前发布开始时刻，不访问宿主业务状态。
    void start() noexcept {
        started = std::chrono::steady_clock::now();
        has_started.store(true,std::memory_order_release);
    }
    // 在调用返回或抛错后发布结束事实，不表示宿主已经接收或提交结果。
    void finish(CallOutcome value) noexcept {
        finished = std::chrono::steady_clock::now(); outcome = value;
        has_finished.store(true,std::memory_order_release);
    }
};
// 在提交后台前准备独立请求身份及固定大小诊断记录。
ModelRequest::ModelRequest() : id_(next_domain()), progress_(std::make_shared<CallProgress>()) {}
// 取得同一请求的时间快照，结束事实可见时开始事实必然也可见。
// 步骤：读取已发布的结束字段 → 读取开始字段 → 返回不含调用内容的值副本。
CallDiagnostics ModelRequest::diagnostics() const noexcept {
    CallDiagnostics result; result.request = id_; result.submitted = progress_->submitted;
    if (progress_->has_finished.load(std::memory_order_acquire)) {
        result.finished = progress_->finished; result.outcome = progress_->outcome;
    }
    if (progress_->has_started.load(std::memory_order_acquire)) result.started = progress_->started;
    return result;
}
// 仅以完整文本结束的成功响应提供旧接口，工具调用不能冒充空回答。
const std::string& ModelRequest::answer() const {
    const auto& value = response();
    if (value.finish != FinishReason::stop || !value.message.tool_calls.empty()) throw std::logic_error("model: response is not final text");
    return value.message.content;
}
// 保留模型错误与取消边界，借用响应必须保持完整请求。
const Response& ModelRequest::response() const {
    if (operation()->state() == OperationState::failed) std::rethrow_exception(error_);
    if (operation()->state() == OperationState::cancelled) throw std::runtime_error("model: cancelled");
    if (operation()->state() != OperationState::succeeded) throw std::logic_error("model: request pending");
    return response_;
}
// 固定请求容量并选择内置 Provider 或明确提供的测试调用。
AsyncClient::AsyncClient(std::size_t capacity, Completion completion)
    : AsyncClient(capacity,Backend{[completion = completion ? std::move(completion) : Completion(
        [](const Config& config,const std::vector<Message>& messages) { return complete(config,messages); })]
        (const Config& config,const Request& request) { return Response{{Role::assistant,completion(config,request.messages)}}; }}) {
    structured_ = false;
}
// 准备有界请求槽并保留显式后端，空注入不会回退为真实网络调用。
AsyncClient::AsyncClient(std::size_t capacity, Backend backend)
    : requests_(capacity), backend_(std::move(backend)), structured_(backend_.structured) {
    if (!capacity || !backend_.complete) throw std::invalid_argument("model: positive capacity and backend required");
}
// 不丢弃进行中的后台调用；已取消业务仍等待真实 I/O 返回后才释放所用输入和配置。
AsyncClient::~AsyncClient() { wait_closed(); }
// 最外层兜底等待已接受的异步调用完成，不要求标准库工作线程池整体退出。
// 步骤：关闭接纳并请求取消 → 等待各调用返回 → 在宿主结算结果及退出确认。
void AsyncClient::wait_closed() {
    close();
    for (const auto& request : requests_) if (request && request->worker_.valid()) request->worker_.wait();
    advance();
}
// 将旧文本消息转入同一异步入口，不静默忽略结构化字段。
std::shared_ptr<ModelRequest> AsyncClient::submit(Config config, std::vector<Message> messages) {
    collect();
    for (const auto& message : messages) if ((message.role != Role::user && message.role != Role::assistant) ||
        !message.tool_calls.empty() || !message.tool_call_id.empty()) throw std::invalid_argument("model: text messages required");
    return submit_request(std::move(config),Request{std::move(messages)});
}
// 将完整输入独立转交后台，异步提交之后只有不会失败的槽位提交。
// 步骤：回收并校验接纳、协议及后端能力 → 准备结果及输入捕获 → 提交后台调用 → 保存宿主责任。
std::shared_ptr<ModelRequest> AsyncClient::submit_request(Config config, Request input) {
    collect();
    if (!accepting_) throw std::logic_error("model: async admission closed");
    validate_request(input);
    if (!structured_) {
        if (!input.tools.empty() || input.max_output_tokens) throw std::invalid_argument("model: text backend does not support request options");
        for (const auto& message : input.messages) if ((message.role != Role::user && message.role != Role::assistant) ||
            !message.tool_calls.empty() || !message.tool_call_id.empty()) throw std::invalid_argument("model: text backend does not support structured messages");
    }
    for (auto& slot : requests_) if (!slot) {
        auto request = std::shared_ptr<ModelRequest>(new ModelRequest());
        request->progress_->submitted = std::chrono::steady_clock::now();
        request->worker_ = std::async(std::launch::async,
        // 在后台发布调用阶段，完整响应或原异常仍仅通过 future 交还宿主。
        // 步骤：发布开始 → 调用供应商 → 按返回或抛错发布结束 → 交还原结果。
        [progress = request->progress_,backend = backend_,config = std::move(config),input = std::move(input)] {
            progress->start();
            try {
                auto response = backend.complete(config,input);
                progress->finish(CallOutcome::returned); return response;
            } catch (...) { progress->finish(CallOutcome::threw); throw; }
        });
        slot = request; ++size_; return request;
    }
    throw std::length_error("model: async request capacity exhausted");
}
// 宿主裁定取消优先于同次推进观察到的回复，工作线程只交还完整响应或异常。
// 步骤：回收旧结果 → 结算协作取消 → 非阻塞检查工作退出 → 接受或丢弃回复 → 确认实际退出。
void AsyncClient::advance() {
    collect();
    for (const auto& request : requests_) {
        if (!request || request->exit()->state() != OperationState::pending) continue;
        if (request->operation()->state() == OperationState::pending && request->operation()->cancellation_reason() != CancelReason::none)
            request->result_.settle(OperationState::cancelled);
        if (request->worker_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) continue;
        try {
            auto response = request->worker_.get();
            if (request->operation()->state() == OperationState::pending) {
                validate_response(response);
                request->response_ = std::move(response); request->result_.settle(OperationState::succeeded);
            }
        } catch (...) {
            if (request->operation()->state() == OperationState::pending) {
                request->error_ = std::current_exception(); request->result_.settle(OperationState::failed);
            }
        }
        request->exit_.settle(OperationState::succeeded);
    }
}
// 关闭接纳并请求取消，已经完成的结果及首个取消原因保持不变。
// 步骤：停止新请求 → 登记各请求停止原因 → 非阻塞推进已有结果。
void AsyncClient::close() {
    accepting_ = false;
    for (const auto& request : requests_) if (request) request->request_cancel(CancelReason::owner_stop);
    advance();
}
// 只以后台实际退出判断关闭，不以业务取消或客户端释放替代。
bool AsyncClient::closed() const noexcept {
    if (accepting_) return false;
    for (const auto& request : requests_) if (request && request->exit()->state() == OperationState::pending) return false;
    return true;
}
// 仅归还没有客户端持有的退出记录，保留结果也继续占请求容量。
void AsyncClient::collect() noexcept {
    for (auto& request : requests_) if (request && request->exit()->state() != OperationState::pending && request.use_count() == 1) {
        request.reset(); --size_;
    }
}
}
