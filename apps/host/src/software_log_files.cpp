#include "software_log_files.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <regex>
#include <system_error>
#include <tuple>
#include <vector>

namespace dfy::app {
namespace {
std::atomic_uint64_t serial{0};
struct Candidate { std::filesystem::path path; std::uint64_t size, modified; };

// 用 Windows 的两个 32 位字段恢复文件长度或写入时刻。
std::uint64_t wide(DWORD high, DWORD low) { return (static_cast<std::uint64_t>(high)<<32) | low; }

// 保留 Windows 提供的写盘或枚举错误码。
void io_failure(DWORD code) { throw std::system_error(static_cast<int>(code),std::system_category()); }
}

// 固定每次启动的命名前缀，在首次写入前核对目录和保留预算。
// 步骤：检查段大小与总预算 → 生成独立前缀 → 创建日志目录。
SoftwareLogFiles::SoftwareLogFiles(const std::filesystem::path& folder, SoftwareLogLimits limits)
    : folder_(folder), limits_(limits) {
    if (!limits_.segment_bytes || limits_.retained_bytes < limits_.segment_bytes || !limits_.retained_files)
        throw std::invalid_argument("software log: invalid retention limits");
    const auto stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    stem_ = "software-"+std::to_string(stamp)+"-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(++serial);
    std::filesystem::create_directories(folder_);
}

// 核对本目录内产品文件的数量与字节数，避开活动句柄、重解析点和其他文件。
// 步骤：采集普通日志文件 → 按写入时间排序 → 在两个预算内从最老的可回收文件开始清理。
void SoftwareLogFiles::trim(bool reserve_segment) const {
    static const std::wregex name(LR"(^software-[0-9]+-[0-9]+-[0-9]+-[0-9]+\.jsonl$)");
    WIN32_FIND_DATAW info{};
    const HANDLE raw = FindFirstFileW((folder_/"software-*.jsonl").c_str(),&info);
    if (raw == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) return;
        io_failure(error);
    }
    const std::unique_ptr<void,decltype(&FindClose)> search(raw,&FindClose);
    std::vector<Candidate> files;
    std::uint64_t total = 0;
    do {
        if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) ||
            !std::regex_match(info.cFileName,name)) continue;
        auto path = folder_/info.cFileName;
        const auto size = path==current_ && reserve_segment ? limits_.segment_bytes : wide(info.nFileSizeHigh,info.nFileSizeLow);
        files.push_back({std::move(path),size,wide(info.ftLastWriteTime.dwHighDateTime,info.ftLastWriteTime.dwLowDateTime)});
        total += size;
    } while (FindNextFileW(search.get(),&info));
    const auto error = GetLastError();
    if (error != ERROR_NO_MORE_FILES) io_failure(error);

    std::ranges::sort(files,[](const Candidate& a, const Candidate& b) {
        return std::tie(a.modified,a.path) < std::tie(b.modified,b.path);
    });
    auto count = files.size();
    for (const auto& file : files) {
        if (count <= limits_.retained_files && total <= limits_.retained_bytes) break;
        if (reserve_segment && file.path == current_) continue;
        if (DeleteFileW(file.path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) {
            --count;
            total -= file.size;
        }
    }
}

// 打开新段后归还旧段句柄，其他进程也能按统一预算回收它。
// 步骤：独占创建新段 → 切换当前句柄 → 为本段预留空间。
void SoftwareLogFiles::next_segment() {
    const auto next = folder_/(stem_+"-"+std::to_string(++segment_)+".jsonl");
    const auto raw = CreateFileW(next.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (raw == INVALID_HANDLE_VALUE) io_failure(GetLastError());
    file_.reset(raw);
    current_ = next;
    bytes_ = 0;
    trim(true);
}

// 在段边界保留 JSONL 整行，完成写入后立即刷新系统缓冲。
// 步骤：按大小决定换段 → 写入并核对长度 → 刷新可见记录。
void SoftwareLogFiles::append(std::string_view line) {
    if (line.size() > limits_.segment_bytes || line.size() > MAXDWORD) io_failure(ERROR_BUFFER_OVERFLOW);
    if (!file_ || line.size() > limits_.segment_bytes-bytes_) next_segment();
    DWORD written = 0;
    if (!WriteFile(file_.get(),line.data(),static_cast<DWORD>(line.size()),&written,nullptr)) io_failure(GetLastError());
    if (written != line.size()) io_failure(ERROR_WRITE_FAULT);
    bytes_ += written;
    if (!FlushFileBuffers(file_.get())) io_failure(GetLastError());
}

// 正常收尾时按段的实际长度重算保留量，最后一个退出的写入者也会归还预算。
void SoftwareLogFiles::close() {
    file_.reset();
    if (!current_.empty()) trim(false);
}
}
