#pragma once
#include <dafeiyu/diagnostic_snapshot.hpp>

namespace dfy::agent {
// 编码有版本的只读诊断，时刻转为相对纳秒；最多 8 MiB，不写文件或上传。
std::string encode_diagnostic(const DiagnosticSnapshot& snapshot);
// 只读取本格式的有界诊断数据并校验关联，未知字段／版本拒绝，不恢复任何执行对象。
DiagnosticSnapshot decode_diagnostic(std::string_view text);
}
