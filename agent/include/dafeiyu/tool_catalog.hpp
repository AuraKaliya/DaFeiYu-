#pragma once
#include <dafeiyu/model_messages.hpp>
#include <dafeiyu/runtime_ids.hpp>
#include <dafeiyu/tool_work.hpp>
#include <memory>
#include <span>
#include <utility>

namespace dfy::agent {
struct ToolDescription {
    std::string id;
    runtime::DomainId instance = 0;
    std::uint32_t version = 1;
    model::ToolDefinition model;
    bool changes_state = false;
    bool idempotent = false;
    std::optional<std::string> plugin_version, schema_digest;
};
struct ToolSelection { ToolDescription description; runtime::DomainId registration = 0; };
struct ToolEntry;
class ToolCatalog;
// 单个运行的固定成员快照；撤销与当前授权仍在每次解析时核对。
class ToolSnapshot {
  public:
    // 返回仍获准且已绑定实现的成员描述，不追加快照建立后注册的工具。
    std::vector<model::ToolDefinition> definitions() const;
    // 复制公开描述；默认只返回当前可用成员，版本清单可显式读取原固定成员，不取得工作或原生代码。
    std::vector<ToolDescription> descriptions(bool available_only = true) const;
    // 判断固定授予成员是否包含变更工具，禁用或撤销不会隐藏原范围的性质。
    bool permits_state_changes() const noexcept;
    // 从固定成员收紧步骤范围，保留原注册及撤销关系；不能补回外部工具或重绑新实例。
    ToolSnapshot restrict_to(std::span<const std::string> ids) const;
    // 收紧到一个工具及一份已授权参数，后续缩小范围仍保留原值约束。
    ToolSnapshot bind_arguments(const std::string& id, const std::string& arguments) const;
    // 为支持信封的工具附加宿主上下文，模型仍只能填写原工具契约中的参数。
    ToolSnapshot bind_context(const std::string& context) const;
    // 按模型名称解析已授权工具并校验参数，旧实例和已禁用成员拒绝。
    ToolSelection resolve(const std::string& name, const std::string& arguments) const;
    // 校验调用并准备延后执行的驱动，首次实际调用前再次核对原注册授权。
    std::unique_ptr<ToolDriver> prepare(AttemptId attempt, model::ToolCall call, ToolResources resources = {}) const;
  private:
    friend class ToolCatalog;
    // 保留已完整校验的成员集合，不取得任何原生函数指针。
    explicit ToolSnapshot(std::vector<std::shared_ptr<ToolEntry>> entries) : entries_(std::move(entries)) {}
    std::vector<std::shared_ptr<ToolEntry>> entries_;
    std::shared_ptr<const std::string> fixed_arguments_;
    std::shared_ptr<const std::string> context_;
};
// 同一宿主线程内的有界描述目录，宿主授予工具 ID，模型只返回候选调用。
class ToolCatalog {
  public:
    // 限制目录为一至 64 项，校验容量后分配固定注册槽。
    explicit ToolCatalog(std::size_t capacity = 64);
    // 撤销所有仍由本目录拥有的注册，使外部快照不能延长授权。
    ~ToolCatalog();
    // 禁止复制目录的注册和撤销责任。
    ToolCatalog(const ToolCatalog&) = delete;
    // 禁止赋值或移动覆盖活动目录。
    ToolCatalog& operator=(const ToolCatalog&) = delete;
    // 完整校验后注册描述和可选工厂；未绑定实现的描述不会向模型暴露。
    runtime::DomainId add(ToolDescription description, ToolFactory factory = {});
    // 撤销并释放注册槽，旧快照仍保留描述但不能再解析执行目标。
    void remove(runtime::DomainId registration);
    // 改变当前授权开关，不改变已签发身份或旧快照成员集合。
    void enable(runtime::DomainId registration, bool enabled);
    // 按显式 ID 列表生成快照；变更状态的工具必须得到额外明确授权。
    ToolSnapshot snapshot(std::span<const std::string> ids, bool allow_changes = false) const;
  private:
    // 查找当前注册；未知和已经撤销的身份拒绝，禁止按相同名称回退到新实例。
    std::shared_ptr<ToolEntry> find(runtime::DomainId registration) const;
    std::vector<std::shared_ptr<ToolEntry>> entries_;
};
}
