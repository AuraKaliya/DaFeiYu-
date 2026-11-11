#pragma once
#include <dafeiyu/model_client.hpp>
#include <dafeiyu/events.hpp>

namespace dfy::agent {
struct Notifications {
    events::PublishResult started;
    events::PublishResult finished;
};

// 创建和操作均限同一宿主线程；会话及借用的总线必须活到当前操作结束。
class Session {
  public:
    // 保存已校验配置并借用宿主总线，分配进程内不复用的会话标识。
    Session(model::Config config, events::Bus& events);
    // 禁止复制会话，避免复制身份、历史和操作状态；同时不提供移动操作。
    Session(const Session&) = delete;
    // 禁止赋值，保持会话身份和总线归属稳定。
    Session& operator=(const Session&) = delete;

    // 拒绝会话或总线忙碌时的调用；接受后发布开始及唯一终态，失败抛出原异常。
    std::string ask(const std::string& prompt);

    // 空闲时清空历史后通知，不重置回合号；忙碌时抛出异常且不改变历史。
    void clear();

    // 返回当前进程内的会话标识，供宿主关联事件。
    std::uint64_t id() const noexcept { return id_; }
    // 返回已提交的历史消息数量，允许观察者在通知期间读取提交状态。
    std::size_t message_count() const noexcept { return history_.size(); }
    // 返回最近一次已接受操作的通知报告；操作返回后完整，拒绝重入不覆盖报告。
    Notifications last_notifications() const noexcept { return notifications_; }

  private:
    model::Config config_;
    events::Bus& events_;
    std::uint64_t id_;
    std::uint64_t turn_ = 0;
    bool busy_ = false;
    Notifications notifications_;
    std::vector<model::Message> history_;
};
}
