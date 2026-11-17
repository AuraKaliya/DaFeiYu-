#pragma once
#include <dafeiyu/diagnostic_snapshot.hpp>

namespace dfy::agent {
// 编码有版本的只读诊断，时刻转为相对纳秒；最多 8 MiB，不写文件或上传。
std::string encode_diagnostic(const DiagnosticSnapshot& snapshot);
}
