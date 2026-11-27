#pragma once
#include <filesystem>
#include <string>

namespace dfy::tests {
// 将这个测试临时档案还原为指定旧版实际具备的表。
void legacy_archive(const std::filesystem::path& directory,unsigned version);
// 检查不明确或未来版本的测试档案不会以任意身份启动，事务不会留下半升级状态。
void check_unusable_archive(const std::filesystem::path& temporary,const std::string& fault);
// 在临时档案中还原旧实现跨授权续接后写出的结束信及其关联索引。
void legacy_letter_scope(const std::filesystem::path& directory,const std::string& letter,const std::string& source);
}
