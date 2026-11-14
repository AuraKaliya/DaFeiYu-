#include <dafeiyu/async_model.hpp>
#include <stdexcept>

namespace dfy::model {
using namespace runtime;
// 保留真实模型错误边界，借用文本必须保持完整请求。
const std::string& ModelRequest::answer() const {
    if (operation()->state() == OperationState::failed) std::rethrow_exception(error_);
    if (operation()->state() == OperationState::cancelled) throw std::runtime_error("model: cancelled");
    if (operation()->state() != OperationState::succeeded) throw std::logic_error("model: request pending");
    return answer_;
}
// 固定请求容量并选择内置 Provider 或明确提供的测试调用。
AsyncClient::AsyncClient(std::size_t capacity, Completion completion)
    : requests_(capacity), completion_(completion ? std::move(completion) : Completion([](const Config& config,const std::vector<Message>& messages) {
        return complete(config,messages);
    })) {
    if (!capacity) throw std::invalid_argument("model: positive async capacity required");
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
// 将完整输入独立转交后台，异步提交之后只有不会失败的槽位提交。
// 步骤：回收并检查接纳及完整消息约束 → 准备结果及输入捕获 → 提交后台调用 → 保存宿主责任。
std::shared_ptr<ModelRequest> AsyncClient::submit(Config config, std::vector<Message> messages) {
    collect();
    if (!accepting_) throw std::logic_error("model: async admission closed");
    std::size_t bytes = 0;
    if (messages.empty()) throw std::invalid_argument("model: messages required");
    for (const auto& message : messages) {
        if ((message.role != Role::user && message.role != Role::assistant) || message.content.empty() ||
            (message.role == Role::user && message.content.find_first_not_of(" \t\r\n") == std::string::npos))
            throw std::invalid_argument("model: valid role and nonempty message required");
        if (message.content.size() > 1024 * 1024 - bytes) throw std::length_error("model: messages exceed 1 MiB");
        bytes += message.content.size();
    }
    for (auto& slot : requests_) if (!slot) {
        auto request = std::shared_ptr<ModelRequest>(new ModelRequest());
        request->worker_ = std::async(std::launch::async,[completion = completion_,config = std::move(config),messages = std::move(messages)] {
            return completion(config,messages);
        });
        slot = request; ++size_; return request;
    }
    throw std::length_error("model: async request capacity exhausted");
}
// 宿主裁定取消优先于同次推进观察到的回复，工作线程只交还完整文本或异常。
// 步骤：回收旧结果 → 结算协作取消 → 非阻塞检查工作退出 → 接受或丢弃回复 → 确认实际退出。
void AsyncClient::advance() {
    collect();
    for (const auto& request : requests_) {
        if (!request || request->exit()->state() != OperationState::pending) continue;
        if (request->operation()->state() == OperationState::pending && request->operation()->cancellation_reason() != CancelReason::none)
            request->result_.settle(OperationState::cancelled);
        if (request->worker_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) continue;
        try {
            auto answer = request->worker_.get();
            if (request->operation()->state() == OperationState::pending) {
                if (answer.empty() || answer.size() > 8 * 1024 * 1024) throw std::length_error("model: complete text outside response limit");
                request->answer_ = std::move(answer); request->result_.settle(OperationState::succeeded);
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
