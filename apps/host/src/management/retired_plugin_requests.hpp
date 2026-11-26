#pragma once
#include <string>

struct sqlite3;
namespace dfy::management {
// 当前后台已退休的公开提交号，回执正文仍使用宿主的有界保留区。
class RetiredPluginRequests {
  public:
    // 首次退休公开回执前不打开临时数据库。
    RetiredPluginRequests() = default;
    // 随后台关闭并归还临时数据库，旧实例的提交须经实例号拒绝。
    ~RetiredPluginRequests();
    // 临时数据库连接仅归本宿主所有。
    RetiredPluginRequests(const RetiredPluginRequests&) = delete;
    // 禁止复制覆盖连接及其退休记录。
    RetiredPluginRequests& operator=(const RetiredPluginRequests&) = delete;
    // 在宿主锁内登记已接纳的提交号，登记成功后才允许回收其详细回执。
    void remember(const std::string& submission);
    // 在宿主锁内区分从未接纳与已退休的公开身份，不创建数据库。
    bool contains(const std::string& submission) const;
  private:
    sqlite3* db_ = nullptr;
};
}
