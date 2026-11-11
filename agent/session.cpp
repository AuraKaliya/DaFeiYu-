#include <dafeiyu/session.hpp>
#include <limits>
#include <stdexcept>
#include <utility>

namespace dfy::agent {
namespace {
// 在宿主线程分配非零会话标识，耗尽时拒绝创建，避免回绕复用身份。
std::uint64_t next_session_id() {
    static std::uint64_t next = 1;
    if (next == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("session: identifiers exhausted");
    }
    return next++;
}

class ActiveCall {
  public:
    // 占用会话直到本次操作返回，拒绝会话重入或共享总线上的嵌套通知。
    // 步骤：检查会话与总线状态 → 标记会话忙碌。
    ActiveCall(bool& busy, const events::Bus& events) : busy_(busy) {
        if (busy || events.publishing()) {
            throw std::runtime_error("session: busy");
        }
        busy_ = true;
    }
    // 在正常或异常退出时恢复会话空闲状态。
    ~ActiveCall() { busy_ = false; }
    // 禁止复制，保持忙碌状态的恢复责任唯一。
    ActiveCall(const ActiveCall&) = delete;
    // 禁止赋值，避免改变当前操作的状态归属。
    ActiveCall& operator=(const ActiveCall&) = delete;

  private:
    bool& busy_;
};
}

// 保存模型配置和宿主总线引用，创建具有独立身份的空会话。
Session::Session(model::Config config, events::Bus& events)
    : config_(std::move(config)), events_(events), id_(next_session_id()) {}

// 在已有上下文上完成问答，通知失败与模型结果分开记录。
// 步骤：拒绝重入并接受新回合 → 通知开始 → 构造新历史并调用模型 → 提交成功或保留原历史后通知终态。
std::string Session::ask(const std::string& prompt) {
    const ActiveCall active(busy_, events_);
    if (turn_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("session: turn identifiers exhausted");
    }
    const auto turn = ++turn_;
    notifications_ = {};
    notifications_.started = events_.publish({events::Kind::turn_started, id_, turn});

    std::string answer;
    try {
        auto pending = history_;
        pending.push_back({model::Role::user, prompt});
        answer = model::complete(config_, pending);

        // 先构造完整的新历史，再交换提交，避免异常留下半轮对话。
        pending.push_back({model::Role::assistant, answer});
        history_.swap(pending);
    } catch (...) {
        notifications_.finished = events_.publish({events::Kind::turn_failed, id_, turn});
        throw;
    }

    // 成功通知位于业务异常边界外，回调抛错不会制造第二个终态或回滚已提交历史。
    notifications_.finished = events_.publish({events::Kind::turn_completed, id_, turn});
    return answer;
}

// 清空已提交历史并发布独立通知，保留模型配置与回合编号。
// 步骤：拒绝重入 → 清空历史 → 以零回合标识通知清空结果。
void Session::clear() {
    const ActiveCall active(busy_, events_);
    notifications_ = {};
    history_.clear();

    notifications_.finished = events_.publish({events::Kind::session_cleared, id_, 0});
}
}
