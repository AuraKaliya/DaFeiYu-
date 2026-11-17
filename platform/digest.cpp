#include <dafeiyu/digest.hpp>
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <limits>
#include <stdexcept>
#include <vector>

namespace dfy::platform {
namespace {
// 拒绝系统哈希失败，不把待摘要内容加入错误信息。
void checked(NTSTATUS status) { if (status < 0) throw std::runtime_error("digest: SHA-256 operation failed"); }
struct Handles {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    // 创建空的系统句柄责任。
    Handles() = default;
    // 按依赖顺序释放哈希及算法，不抛出清理异常。
    ~Handles() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm,0); }
    // 禁止复制关闭责任。
    Handles(const Handles&) = delete;
    // 禁止赋值覆盖系统句柄。
    Handles& operator=(const Handles&) = delete;
};
}
// 以系统实现计算确定摘要，哈希对象销毁前始终保留其缓冲区。
// 步骤：核对长度并打开算法 → 准备有界哈希对象 → 处理原字节并完成 → 返回十六进制值。
std::string sha256(std::string_view bytes) {
    if (bytes.size() > std::numeric_limits<ULONG>::max()) throw std::length_error("digest: input exceeds system limit");
    std::vector<UCHAR> object; Handles handles;
    checked(BCryptOpenAlgorithmProvider(&handles.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0));
    ULONG length = 0, returned = 0;
    checked(BCryptGetProperty(handles.algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),sizeof(length),&returned,0));
    if (returned != sizeof(length) || !length || length > 1024*1024) throw std::runtime_error("digest: invalid provider buffer");
    object.resize(length); checked(BCryptCreateHash(handles.algorithm,&handles.hash,object.data(),length,nullptr,0,0));
    if (!bytes.empty()) checked(BCryptHashData(handles.hash,reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),static_cast<ULONG>(bytes.size()),0));
    std::array<UCHAR,32> result{}; checked(BCryptFinishHash(handles.hash,result.data(),static_cast<ULONG>(result.size()),0));
    constexpr char hex[] = "0123456789abcdef"; std::string text; text.reserve(64);
    for (const auto byte : result) { text.push_back(hex[byte >> 4]); text.push_back(hex[byte & 15]); }
    return text;
}
}
