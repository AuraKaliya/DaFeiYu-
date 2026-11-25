#pragma once
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace dfy::app {
struct SoftwareLogLimits {
    std::uint64_t segment_bytes = 1024*1024;
    std::uint64_t retained_bytes = 16*1024*1024;
    std::size_t retained_files = 64;
};

// 单个日志线程持有当前段，其他进程可以读取但只能回收已关闭的段。
class SoftwareLogFiles {
  public:
    // 创建日志目录，为本次启动签发文件名前缀并核对保留预算。
    explicit SoftwareLogFiles(const std::filesystem::path& folder, SoftwareLogLimits limits = {});
    // 按完整记录换段，成功时本条记录已经写出。
    void append(std::string_view line);
    // 释放活动文件并按实际落盘大小再核对目录预算。
    void close();
  private:
    // 仅创建新文件，写入新段前给完整一段预留空间。
    void next_segment();
    // 按最近写入时间回收本目录内已关闭的产品日志。
    void trim(bool reserve_segment) const;
    std::filesystem::path folder_, current_;
    SoftwareLogLimits limits_;
    std::string stem_;
    std::uint64_t segment_ = 0, bytes_ = 0;
    std::unique_ptr<void,decltype(&CloseHandle)> file_{nullptr,&CloseHandle};
};
}
