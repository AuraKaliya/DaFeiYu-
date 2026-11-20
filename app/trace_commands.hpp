#pragma once
#include <dafeiyu/run_identity.hpp>
#include <filesystem>
#include <iosfwd>
#include <optional>

namespace dfy::app {
// 解析终端中的正整数身份，保留进程内的完整位宽。
runtime::DomainId trace_id(const std::filesystem::path& value);
// 只读列出日志中的运行或打印指定运行，缺尾时保留早期可读事实。
void review_trace(const std::filesystem::path& file, std::optional<agent::RunId> run, std::ostream& output);
}
