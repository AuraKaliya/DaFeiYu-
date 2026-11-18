#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <vector>

namespace dfy::tests::links {
namespace fs = std::filesystem;

// 将真实 NTFS 重解析记录写入本次测试创建的路径。
// 步骤：打开原路径本身 → 写入记录并关闭句柄 → 核对实际重解析属性。
inline void attach_reparse(const fs::path& path, void* buffer, DWORD bytes) {
    const auto file = CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if (file == INVALID_HANDLE_VALUE) throw fs::filesystem_error("open test reparse object",path,{static_cast<int>(GetLastError()),std::system_category()});
    DWORD returned = 0;
    const auto ok = DeviceIoControl(file,FSCTL_SET_REPARSE_POINT,buffer,bytes,nullptr,0,&returned,nullptr);
    const auto error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (error) throw fs::filesystem_error("attach test reparse object",path,{static_cast<int>(error),std::system_category()});
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("test reparse attribute is absent");
}

// 将有效测试文件副本变为真实的非符号链接重解析点。
// 步骤：保留测试正文并创建新路径 → 附加私有标记并核对真实属性 → 失败仅清理本次副本。
inline void marked_file(const fs::path& target, const fs::path& link) {
    if (!fs::copy_file(target,link)) throw std::runtime_error("test reparse fixture already exists");

    try {
        // 私有测试标签只标记本地夹具；不作为重定向目标或生产契约。
        REPARSE_GUID_DATA_BUFFER marker{};
        marker.ReparseTag = 0x2a;
        marker.ReparseDataLength = 1;
        marker.ReparseGuid = {0x08f8dc0c,0xdd5e,0x4fa9,{0xb5,0x2c,0x6a,0xc1,0x22,0xc7,0x40,0x0b}};
        marker.GenericReparseBuffer.DataBuffer[0] = 1;
        attach_reparse(link,&marker,REPARSE_GUID_DATA_BUFFER_HEADER_SIZE+1);
    } catch (...) { std::error_code ignored; fs::remove(link,ignored); throw; }
}

// 保留真实文件符号链接测试；特权不足时验证同一原生重解析拒绝策略。
// 步骤：尝试原符号链接 → 区分失败原因 → 仅在特权不足时创建标记夹具。
inline void file(const fs::path& target, const fs::path& link) {
    std::error_code error;
    fs::create_symlink(target,link,error);
    if (!error) return;
    if (error.value() != ERROR_PRIVILEGE_NOT_HELD) throw fs::filesystem_error("create file symlink",target,link,error);
    marked_file(target,link);
    std::clog << "File rejection uses an NTFS reparse marker (no symlink privilege).\n";
}

// 保留目录重定向行为；普通账户使用系统支持的 NTFS 目录联接。
// 步骤：尝试原符号链接 → 编码同一目标的联接 → 创建并确认真实重解析属性。
inline void directory(const fs::path& target, const fs::path& link) {
    std::error_code error;
    fs::create_directory_symlink(target,link,error);
    if (!error) return;
    if (error.value() != ERROR_PRIVILEGE_NOT_HELD) throw fs::filesystem_error("create directory symlink",target,link,error);

    struct Junction {
        DWORD tag;
        WORD length, reserved, target_offset, target_size, print_offset, print_size;
    };
    static_assert(sizeof(Junction) == 16);
    const auto print = fs::absolute(target).lexically_normal().make_preferred().native();
    const auto native = L"\\??\\"+print;
    const auto target_bytes = native.size()*sizeof(wchar_t), print_bytes = print.size()*sizeof(wchar_t);
    const auto name_bytes = target_bytes+print_bytes+2*sizeof(wchar_t);
    if (name_bytes+sizeof(Junction) > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) throw std::length_error("test junction target too long");
    const Junction header{IO_REPARSE_TAG_MOUNT_POINT,static_cast<WORD>(name_bytes+8),0,0,static_cast<WORD>(target_bytes),
        static_cast<WORD>(target_bytes+sizeof(wchar_t)),static_cast<WORD>(print_bytes)};
    std::vector<BYTE> value(sizeof(header)+name_bytes);
    std::memcpy(value.data(),&header,sizeof(header));
    std::memcpy(value.data()+sizeof(header),native.c_str(),target_bytes);
    std::memcpy(value.data()+sizeof(header)+header.print_offset,print.c_str(),print_bytes);
    if (!fs::create_directory(link)) throw std::runtime_error("test junction already exists");
    try { attach_reparse(link,value.data(),static_cast<DWORD>(value.size())); }
    catch (...) { fs::remove(link,error); throw; }
    if (!fs::equivalent(target,link)) throw std::runtime_error("test junction did not redirect to its target");
    std::clog << "Directory rejection uses a real NTFS junction.\n";
}
}
