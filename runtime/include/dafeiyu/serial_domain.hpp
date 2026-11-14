#pragma once
#include <dafeiyu/operation.hpp>
#include <dafeiyu/runtime_ids.hpp>
#include <atomic>
#include <optional>
#include <vector>

namespace dfy::runtime {
// 宿主内部的顺序占用阶段；授予不等于业务完成，释放不表达业务成功。
enum class SerialPhase { waiting, granted, running, released, cancelled };
enum class SerialClose { drain, cancel };
class SerialDomain;
class SerialRequest {
  public:
    // 禁止复制排队身份，客户端共享同一只读记录。
    SerialRequest(const SerialRequest&) = delete;
    // 禁止赋值或隐式移动已签发请求。
    SerialRequest& operator=(const SerialRequest&) = delete;
    // 返回授予操作，Succeeded 只表示取得顺序占用，不表示处理结束。
    std::shared_ptr<const Operation> admission() const noexcept { return source_.operation(); }
    // 请求取消尚未授予的等待；授予后返回 already_settled，不取消正在进行的处理。
    CancelDisposition request_cancel(CancelReason reason) { return source_.request_cancel(reason); }
    // 返回独立顺序域身份，不复用事件存储的接纳域。
    DomainId domain() const noexcept { return domain_; }
    // 返回本域接受请求时的非零序号，失败不消耗序号。
    std::uint64_t sequence() const noexcept { return sequence_; }
    // 返回当前占用阶段，查询不推进授予或释放。
    SerialPhase phase() const noexcept { return phase_; }
  private:
    friend class SerialDomain;
    // 创建尚未发布的等待记录，序号在完整分配后由域签发。
    explicit SerialRequest(DomainId domain) : domain_(domain) {}
    const DomainId domain_;
    std::uint64_t sequence_ = 0;
    SerialPhase phase_ = SerialPhase::waiting;
    bool bound_ = false;
    OperationSource source_{true};
};

// 同宿主线程的有界 FIFO 顺序域，宿主必须持有本控制器直至已授予处理真正结束。
class SerialDomain {
  public:
    // 建立独立域及正记录容量，等待、活动占用和客户端保留的终态共同计费。
    explicit SerialDomain(std::size_t capacity);
    // 取消未授予等待，不伪造已授予处理的结束；活动处理应由宿主先显式 finish。
    ~SerialDomain();
    // 禁止复制域身份或授予责任。
    SerialDomain(const SerialDomain&) = delete;
    // 禁止赋值或隐式移动正在使用的顺序域。
    SerialDomain& operator=(const SerialDomain&) = delete;
    // 有界接纳等待记录，不在提交时授予或执行工作。
    std::shared_ptr<SerialRequest> submit();
    // 先处理等待取消，再在没有活动占用时授予最早请求，不调用工作或通知回调。
    void advance();
    // 为一项处理独占绑定本域的未开始请求，重复绑定或已结束请求明确拒绝。
    void bind(const std::shared_ptr<const SerialRequest>& request);
    // 确认已绑定处理实际开始，授予通知或读取完成本身不会进入 running。
    void begin(const std::shared_ptr<const SerialRequest>& request);
    // 撤回已绑定但尚未开始的处理，归还等待或授予占用，不推进下一项。
    void withdraw(const std::shared_ptr<const SerialRequest>& request, CancelReason reason);
    // 宿主确认实际处理结束后释放当前占用，不自动授予后续请求或改写授予操作。
    void finish(const std::shared_ptr<const SerialRequest>& request);
    // 停止接纳并选择排空或取消等待，已授予处理保持占用直到 finish。
    void close(SerialClose mode);
    // 回收无客户端持有的已释放或已取消记录，等待和活动占用不因客户端释放而消失。
    void collect() noexcept;
    // 返回活动记录的独立只读持有，宿主可据此完成无客户端的已授予处理。
    std::shared_ptr<const SerialRequest> active() const noexcept { return active_ ? requests_[*active_] : nullptr; }
    // 返回尚未授予的记录数，取消请求需推进后才退出等待阶段。
    std::size_t pending() const noexcept;
    // 判断是否停止接纳且所有占用及等待已结束，不要求外部终态快照已释放。
    bool closed() const noexcept { return mode_.has_value() && !active_ && !pending(); }
    // 返回已选择的关闭方式，开放时没有关闭方式。
    std::optional<SerialClose> close_mode() const noexcept { return mode_; }
    // 返回占用记录数，包含客户端仍保留的终态快照。
    std::size_t size() const noexcept { return size_; }
    // 返回宿主签发的独立顺序域身份。
    DomainId id() const noexcept { return domain_; }
  private:
    // 核对本域请求身份并取得宿主可变记录，客户端无法自行构造身份。
    SerialRequest& owned(const std::shared_ptr<const SerialRequest>& request) const;
    const DomainId domain_;
    std::vector<std::shared_ptr<SerialRequest>> requests_;
    std::atomic<std::uint64_t> last_sequence_{0};
    std::optional<std::size_t> active_;
    std::optional<SerialClose> mode_;
    std::size_t size_ = 0;
};
}
