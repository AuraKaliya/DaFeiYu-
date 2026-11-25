#pragma once
#include <map>
#include <string>

namespace dfy::agent {
// 宿主按绑定提供的状态事实。字段名和来源由宿主固定，正文不带执行权限。
struct ContextStatus {
    std::string binding;
    std::map<std::string,std::string> fields;
};
// 同一绑定有已提交基线时生成变化或同版引用，否则返回可独立解释的基线。
std::string project_context_status(const ContextStatus* previous, const ContextStatus& current);
}
