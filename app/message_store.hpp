#pragma once
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>
#include <nlohmann/json.hpp>

namespace dfy::app {
struct StoredMessage {
    std::int64_t id, revision, utc_ms;
    std::string source, submission;
    nlohmann::json envelope, receipt;
};
struct MessageAdmission { StoredMessage message; bool created; };
struct MessagePage { std::vector<StoredMessage> records; bool more = false; };

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
    struct State;
    std::unique_ptr<State> state_;
};
}
