#pragma once
#include <dafeiyu/processing.hpp>

namespace dfy::events {
// 同宿主线程的处理接纳域，默认串行；等待、运行及外部保留的终态共同计费。
class ProcessingQueue {
  public:
    // 建立正容量处理槽，串行模式在本队列接受处理时固定独立域顺序。
    explicit ProcessingQueue(std::size_t capacity, bool ordered = true);
    // 取消未开始处理，不以析构代替运行中处理退出；宿主必须持有队列直至 closed。
    ~ProcessingQueue();
    // 禁止复制处理接纳及完成责任。
    ProcessingQueue(const ProcessingQueue&) = delete;
    // 禁止赋值或隐式移动处理队列。
    ProcessingQueue& operator=(const ProcessingQueue&) = delete;
    // 完整准备真实数据和记录后接纳处理，失败不消耗来源数据或顺序序号。
    std::shared_ptr<ProcessingRecord> submit(const EventLease& event);
    // 处理开始前取消并推进顺序授予，不调用消费者、不自动开始业务。
    void advance();
    // 返回一个具有开始资格的宿主持有记录，客户端提前释放不会丢失处理责任。
    std::shared_ptr<ProcessingRecord> ready() const noexcept;
    // 确认本队列指定处理开始；未取得顺序资格时返回 waiting。
    StartResult start(const std::shared_ptr<const ProcessingRecord>& record);
    // 固定指定处理的唯一业务结果，不归还顺序占用。
    void complete(const std::shared_ptr<const ProcessingRecord>& record, runtime::OperationState terminal);
    // 确认指定处理已实际退出，再归还顺序占用。
    void finish(const std::shared_ptr<const ProcessingRecord>& record);
    // 停止接纳并排空或取消；取消运行中处理只登记意图，不提前确认退出。
    void close(CloseMode mode);
    // 回收没有客户端持有的已退出记录，终态数据引用继续受固定容量约束。
    void collect() noexcept;
    // 判断全部已受理处理均实际退出，不要求外部终态和数据已释放。
    bool closed() const noexcept;
    // 返回仍由宿主或客户端持有的处理槽数。
    std::size_t size() const noexcept { return size_; }
    // 返回已经选择的关闭方式，开放时为空。
    std::optional<CloseMode> close_mode() const noexcept { return mode_; }
  private:
    // 按不可复制记录身份定位完成源，错误队列或空记录明确拒绝。
    ProcessingSource& owned(const std::shared_ptr<const ProcessingRecord>& record) const;
    std::vector<std::unique_ptr<ProcessingSource>> sources_;
    std::shared_ptr<runtime::SerialDomain> domain_;
    std::optional<CloseMode> mode_;
    std::size_t size_ = 0;
};
}
