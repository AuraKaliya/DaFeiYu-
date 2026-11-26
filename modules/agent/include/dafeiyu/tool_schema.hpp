#pragma once
#include <memory>
#include <stdexcept>
#include <string>

namespace dfy::agent {
// 只携带宿主构造的有界 JSON 拒绝详情，供调用回执与 Trace 原样核对。
class ToolRejection : public std::invalid_argument {
  public:
    // 保存已经按字段过滤的裁定详情，不转发任意底层异常。
    explicit ToolRejection(const std::string& detail) : std::invalid_argument(detail) {}
};
// 保留容量越界的标准异常类别，同时携带可定位的宿主回执。
class ToolArgumentLimit : public std::length_error {
  public:
    // 保存有界的字段位置和容量规则。
    explicit ToolArgumentLimit(const std::string& detail) : std::length_error(detail) {}
};
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
