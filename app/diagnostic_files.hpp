#pragma once
#include <dafeiyu/diagnostic_json.hpp>
#include <filesystem>

namespace dfy::app {
// 完整编码后写入同目录临时文件，再发布为新文件；已有目标明确拒绝覆盖，不上传。
void write_diagnostic_file(const std::filesystem::path& destination, const agent::DiagnosticSnapshot& value);
// 从明确路径读取最多 8 MiB 的只读诊断；不加载插件、恢复任务或调用模型。
agent::DiagnosticSnapshot read_diagnostic_file(const std::filesystem::path& source);
}
