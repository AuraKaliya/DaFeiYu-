#pragma once
#include <dafeiyu/model_client.hpp>
#include <dafeiyu/operation.hpp>
#include <functional>
#include <future>

namespace dfy::model {
using Completion = std::function<std::string(const Config&,const std::vector<Message>&)>;
class AsyncClient;
// 宿主线程观察的完整文本请求，工作线程不访问操作状态或插件对象。
class ModelRequest {
  public:
    // 禁止复制请求身份和完成责任。
    ModelRequest(const ModelRequest&) = delete;
    // 禁止赋值或隐式移动正在使用的请求。
    ModelRequest& operator=(const ModelRequest&) = delete;
    // 返回业务结果，协作取消可早于实际网络调用退出。
    std::shared_ptr<const runtime::Operation> operation() const noexcept { return result_.operation(); }
    // 返回工作线程已经退出并被宿主回收的确认。
    std::shared_ptr<const runtime::Operation> exit() const noexcept { return exit_.operation(); }
    // 在宿主线程记录取消；不强行终止系统网络调用，迟到回复不会恢复已取消结果。
    runtime::CancelDisposition request_cancel(runtime::CancelReason reason) { return result_.request_cancel(reason); }
    // 成功后借用完整文本，失败重抛模型异常，取消或尚未完成明确拒绝。
    const std::string& answer() const;
  private:
    friend class AsyncClient;
    // 准备独立业务和退出操作，尚不启动线程。
    ModelRequest() = default;
    runtime::OperationSource result_{true}, exit_;
    std::future<std::string> worker_;
    std::string answer_;
    std::exception_ptr error_;
};

// 有界完整文本异步适配；后台只调用内置 Provider，所有操作结算限同一宿主线程。
class AsyncClient {
  public:
    // 固定在途及保留结果容量，可注入离线模型；默认沿用随本体发布的完整文本调用。
    explicit AsyncClient(std::size_t capacity = 1, Completion completion = {});
    // 请求取消并等待实际工作线程退出；正常交互应先通过 close 和 advance 非阻塞排空。
    ~AsyncClient();
    // 禁止复制线程及请求回收责任。
    AsyncClient(const AsyncClient&) = delete;
    // 禁止赋值或隐式移动在途客户端。
    AsyncClient& operator=(const AsyncClient&) = delete;
    // 完整准备输入和结果后提交后台调用，容量不足不发起请求，也不内联通知。
    std::shared_ptr<ModelRequest> submit(Config config, std::vector<Message> messages);
    // 非阻塞裁定取消与就绪结果，并确认工作实际退出，不执行消费者回调。
    void advance();
    // 停止新请求接纳并请求取消，实际网络退出仍按原超时边界等待。
    void close();
    // 判断已停止接纳且全部后台调用退出，保留的只读结果不阻止逻辑关闭。
    bool closed() const noexcept;
    // 回收没有客户端持有的退出记录，等待或运行不会随客户端释放消失。
    void collect() noexcept;
    // 返回仍占用的请求槽，包含客户端保留的结果。
    std::size_t size() const noexcept { return size_; }
  private:
    std::vector<std::shared_ptr<ModelRequest>> requests_;
    const Completion completion_;
    std::size_t size_ = 0;
    bool accepting_ = true;
};
}
