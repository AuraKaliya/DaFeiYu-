#pragma once
#include <dafeiyu/runtime_ids.hpp>
#include <array>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace dfy::app {
enum class CompanionSide { primary, game };
enum class CompanionMessageKind { delegation, report, inquiry, answer, failed };
struct CompanionAddress {
    runtime::DomainId session = 0, instance = 0;
    std::string world;
    runtime::DomainId binding = 0;
    // 需要同时匹配会话、原生实例和游戏世界。
    bool operator==(const CompanionAddress&) const = default;
};
struct CompanionMessage {
    CompanionAddress address;
    CompanionSide from;
    CompanionMessageKind kind;
    std::uint64_t revision, command, task, reply_to;
    std::string text;
};
struct CompanionDelta {
    std::uint64_t oldest, latest;
    bool gap;
    std::vector<CompanionMessage> messages;
};

// 只在应用宿主线程使用的有界业务消息记录，调用者身份由投递入口绑定。
class CompanionMessages {
  public:
    // 固定本次会话的记录容量。
    explicit CompanionMessages(std::size_t limit = 64);
    // 切换当前可信来源；历史保持原地址，新世界使用独立发送序号。
    void bind(std::optional<CompanionAddress> current);
    // 核对当前来源、发送方、命令及父消息，返回新记录或已受理的原记录。
    CompanionMessage post(const CompanionAddress& address, CompanionSide from, CompanionMessageKind kind,
        std::uint64_t command, std::uint64_t reply_to, const std::string& text);
    // 查询指定修订后仍保留的消息，超出窗口明确返回缺口。
    CompanionDelta since(std::uint64_t after) const;
    // 返回当前发送方的下一顺序号，供宿主分配内部回报身份。
    std::uint64_t next(CompanionSide side) const;
    // 返回当前委托身份，避免旧游戏观察触发新委托的介入。
    std::uint64_t task() const noexcept { return task_; }
    // 判断已有询问是否仍在等待答复。
    bool awaiting_answer() const noexcept { return inquiry_ != 0; }
  private:
    std::size_t limit_;
    std::optional<CompanionAddress> current_;
    std::deque<CompanionMessage> history_;
    std::array<std::uint64_t, 2> sent_{};
    std::uint64_t latest_ = 0, task_ = 0, inquiry_ = 0, last_binding_ = 0;
};
}
