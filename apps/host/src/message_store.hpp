#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace dfy::app {
// 识别主聊天显式收录格式，返回原句中由主人指定的主题。
std::optional<std::string_view> memory_capture_topic(std::string_view text);
struct StoredMessage {
    std::int64_t id, revision, utc_ms;
    std::string source, submission;
    nlohmann::json envelope, receipt;
};
struct MessageAdmission { StoredMessage message; bool created; };
struct MessagePage { std::vector<StoredMessage> records; bool more = false; };
struct MemoryChange { std::int64_t revision; bool changed, review_due; };

// 每个数据目录拥有一个跨启动的主会话，原文与有序投递回执在同一库内提交。
class MessageStore {
  public:
    // 打开系统 SQLite，事务内初始化首次使用的数据结构与逻辑会话号。
    explicit MessageStore(const std::filesystem::path& profile);
    // 在最后一个使用者释放后关闭数据库连接。
    ~MessageStore();
    // 返回已写入此目录且跨启动稳定的逻辑会话标识。
    const std::string& conversation() const noexcept;
    // 按来源和稳定提交身份读取原文与最新回执。
    std::optional<StoredMessage> find(std::string_view source, std::string_view submission) const;
    // 原命令轮询只读取小份最新回执，不提取消息正文。
    nlohmann::json receipt(std::int64_t message) const;
    // 原子提交原文与首份回执，同号同参返回原结果，同号异参报告冲突。
    MessageAdmission put(std::string_view source, std::string_view submission,
        const nlohmann::json& envelope, const nlohmann::json& receipt);
    // 仅在回执变化时追加提交序号，再原子发布为该消息的最新结果。
    void update(std::int64_t message, const nlohmann::json& receipt);
    // 读取最后一份已提交回执的序号。
    std::int64_t watermark() const;
    // 在固定水位向前补读回执，或以消息号倒序读取当时的历史投影。
    MessagePage page(bool history, std::int64_t cursor, std::int64_t through, unsigned limit) const;
    // 按消息号读取一批尚需核对的投递，已结算历史不进入启动扫描。
    std::vector<StoredMessage> unfinished(std::int64_t after) const;
  private:
    friend class MainMessages;
    // 返回当前档案的记忆授权修订，零表示从未收录原话。
    std::int64_t memory_revision() const;
    // 在一个事务内按原件身份收录或撤权，重复操作保留修订号。
    MemoryChange change_memory(std::string_view identity, const std::optional<std::string>& topic,
        std::string_view excerpt = {}, std::string_view kind = {});
    // 把两份获准原件按重复表达或明确更正建立有来源的修订关系。
    void reflect_memory(const nlohmann::json& comparison);
    // 按主题或稳定投递号读取本体亲历，复盘结论只随完整、仍获准的依据交付。
    nlohmann::json search_experience(const std::vector<std::string>& topics, unsigned count, std::string_view view) const;
    // 核实不同场次的亲历并分别追加解释修订或自主选择变化。
    void review_experience(const nlohmann::json& review);
    // 查询当前档案中最近一次实际接纳的用户原话身份。
    std::string last_memory_source() const;
    // 仅由主人私聊的读取路径按主题或原身份取一份仍获准的原件。
    std::optional<StoredMessage> recall_memory(std::string_view selector) const;
    // 按本档案获准主题选择候选，在整份证据数组的字节预算内保留原件。
    nlohmann::json search_memory(const std::vector<std::string>& topics, unsigned count, std::string_view view) const;
    struct State;
    std::unique_ptr<State> state_;
};
}
