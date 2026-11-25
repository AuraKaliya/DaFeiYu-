#pragma once
#include <memory>
#include <string>

namespace dfy::agent {
// 不可变工具参数契约；仅支持显式子集，未知关键字拒绝而不是忽略。
class ToolSchema {
  public:
    // 校验并保留对象根 schema；最多 64 KiB、八层及每层 32 个属性。
    explicit ToolSchema(std::string json);
    // 复制共享不可变契约；右值也复用此复制，不产生可查询的空契约。
    ToolSchema(const ToolSchema&) = default;
    // 将契约替换为另一有效副本，不改变被共享的定义。
    ToolSchema& operator=(const ToolSchema&) = default;
    // 检查参数类型、必需字段、额外字段和枚举，最多 64 KiB、每个数组 64 项。
    void validate(const std::string& arguments) const;
    // 借用原始 schema 文本，借用期由本对象或其他共享副本覆盖。
    const std::string& json() const noexcept;
  private:
    struct Storage;
    std::shared_ptr<const Storage> storage_;
};
}
