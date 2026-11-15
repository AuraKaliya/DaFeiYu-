#pragma once
#include <dafeiyu/model_client.hpp>
#include <dafeiyu/operation.hpp>
#include <dafeiyu/runtime_ids.hpp>
#include <chrono>
#include <functional>
#include <future>
#include <optional>

namespace dfy::model {
using Completion = std::function<std::string(const Config&,const std::vector<Message>&)>;
// 显式模型入口并声明结构化请求支持；空函数由客户端拒绝，不为遗漏注入的测试选择真实服务。
struct Backend { std::function<Response(const Config&,const Request&)> complete; bool structured = true; };
// 仅描述后台调用返回或抛错，不代替宿主对文本有效性、取消及业务结果的裁定。
enum class CallOutcome { pending, returned, threw };
struct CallDiagnostics {
    using TimePoint = std::chrono::steady_clock::time_point;
    runtime::DomainId request = 0;
    TimePoint submitted;
    std::optional<TimePoint> started, finished;
    CallOutcome outcome = CallOutcome::pending;
};
struct CallProgress;
class AsyncClient;
// 宿主线程观察的完整模型响应，工作线程不访问操作状态或插件对象。
class ModelRequest {
  public:
    // 禁止复制请求身份和完成责任。
    ModelRequest(const ModelRequest&) = delete;
    // 禁止赋值或隐式移动正在使用的请求。
    ModelRequest& operator=(const ModelRequest&) = delete;
    // 返回业务结果，协作取消可早于实际网络调用退出。
    std::shared_ptr<const runtime::Operation> operation() const noexcept { return result_.operation(); }
    // 返回后台调用已完成且宿主已接收结果的确认，不表示标准库线程池整体退出。
    std::shared_ptr<const runtime::Operation> exit() const noexcept { return exit_.operation(); }
    // 在宿主线程记录取消；不强行终止系统网络调用，迟到回复不会恢复已取消结果。
    runtime::CancelDisposition request_cancel(runtime::CancelReason reason) { return result_.request_cancel(reason); }
    // 成功后借用完整文本，失败重抛模型异常，取消或尚未完成明确拒绝。
    const std::string& answer() const;
    // 成功后借用完整结构化响应；协议成功不代表工具已经执行或任务完成。
    const Response& response() const;
    // 在宿主线程读取后台已发布的单调时间快照，不推进结果或持有正文与凭据。
    CallDiagnostics diagnostics() const noexcept;
  private:
    friend class AsyncClient;
    // 准备独立业务和退出操作，尚不提交后台调用。
    ModelRequest();
    const runtime::DomainId id_;
    const std::shared_ptr<CallProgress> progress_;
    runtime::OperationSource result_{true}, exit_;
    std::future<Response> worker_;
    Response response_;
    std::exception_ptr error_;
};

// 有界完整响应异步适配；后台只调用所选模型入口，所有操作结算限同一宿主线程。
class AsyncClient {
  public:
    // 固定在途及保留结果容量，可注入离线模型；默认沿用随本体发布的完整文本调用。
    explicit AsyncClient(std::size_t capacity = 1, Completion completion = {});
    // 固定结果容量并使用明确提供的结构化调用，拒绝空后端。
    AsyncClient(std::size_t capacity, Backend backend);
    // 请求取消并等待所有后台调用实际完成；正常交互应先通过 close 和 advance 非阻塞排空。
    ~AsyncClient();
    // 禁止复制后台调用及请求回收责任。
    AsyncClient(const AsyncClient&) = delete;
    // 禁止赋值或隐式移动在途客户端。
    AsyncClient& operator=(const AsyncClient&) = delete;
    // 完整准备输入和结果后提交后台调用，容量不足不发起请求，也不内联通知。
    std::shared_ptr<ModelRequest> submit(Config config, std::vector<Message> messages);
    // 接纳经过校验的结构化请求，文本替身不支持的字段在提交前拒绝。
    std::shared_ptr<ModelRequest> submit_request(Config config, Request request);
    // 非阻塞裁定取消与就绪结果，并确认工作实际退出，不执行消费者回调。
    void advance();
    // 停止新请求接纳并请求取消，实际网络退出仍按原超时边界等待。
    void close();
    // 关闭后等待已接受调用实际返回，仅用于最外层兜底回收，不用于交互推进。
    void wait_closed();
    // 判断已停止接纳且全部后台调用退出，保留的只读结果不阻止逻辑关闭。
    bool closed() const noexcept;
    // 回收没有客户端持有的退出记录，等待或运行不会随客户端释放消失。
    void collect() noexcept;
    // 返回仍占用的请求槽，包含客户端保留的结果。
    std::size_t size() const noexcept { return size_; }
  private:
    std::vector<std::shared_ptr<ModelRequest>> requests_;
    const Backend backend_;
    bool structured_ = true;
    std::size_t size_ = 0;
    bool accepting_ = true;
};
}
