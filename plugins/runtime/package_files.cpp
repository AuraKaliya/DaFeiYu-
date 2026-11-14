#include "package_files.hpp"
#include <dafeiyu/runtime_ids.hpp>
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <regex>
#include <stdexcept>

namespace dfy::plugins::files {
namespace fs = std::filesystem;
namespace {
// 拒绝设备名、路径分隔、控制字符和 Windows 尾部别名，防止复制到特殊路径。
bool portable(const fs::path& component) {
    const auto bytes = component.u8string(); const std::string value(bytes.begin(),bytes.end());
    static const std::regex reserved(R"((con|prn|aux|nul|conin\$|conout\$|(com|lpt)([1-9]|¹|²|³))(\..*)?)",std::regex::icase);
    return !value.empty() && value != "." && value != ".." && value.back() != '.' && value.back() != ' ' &&
        value.find_first_of("\\/:*?\"<>|") == std::string::npos && !std::regex_match(value,reserved) &&
        std::none_of(value.begin(),value.end(),[](unsigned char c) { return c < 32 || c == 127; });
}
// 直接检查 Windows 属性，拒绝符号链接、目录联接和其他重解析点。
DWORD attributes(const fs::path& path) {
    const auto result = GetFileAttributesW(path.c_str());
    if (result == INVALID_FILE_ATTRIBUTES || (result & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("package: missing path or reparse point");
    return result;
}
// 检查实际解析后的路径仍属于指定父目录，根自身不能成为清理目标。
void within(const fs::path& root, const fs::path& target) {
    const auto relative = fs::canonical(target).lexically_relative(root);
    if (relative.empty() || relative == "." || relative.has_root_path() || *relative.begin() == "..")
        throw std::runtime_error("package: path escaped managed directory");
}
// 核对一个已存在目录确实是管理根的直接子项。
fs::path child(const fs::path& root, const fs::path& target) {
    const auto result = directory(target);
    if (result == root || result.parent_path() != root) throw std::runtime_error("package: expected direct managed child");
    return result;
}
}
// 在任何创建前校验本地路径形式，逐层创建或核对，拒绝盘根和链接祖先。
// 步骤：规范绝对路径 → 校验本地盘及组件 → 逐层核对或创建普通目录 → 返回实际目录。
fs::path directory(const fs::path& path, bool create) {
    if (path.empty()) throw std::invalid_argument("package: directory required");
    auto absolute = fs::absolute(path).lexically_normal();
    if (absolute.filename().empty()) absolute = absolute.parent_path();
    const auto drive = absolute.root_name().native();
    if (drive.size() != 2 || drive[1] != L':' || absolute == absolute.root_path()) throw std::invalid_argument("package: local directory below drive root required");
    auto current = absolute.root_path(); (void)attributes(current);
    for (const auto& component : absolute.relative_path()) {
        if (!portable(component)) throw std::invalid_argument("package: unsafe path component");
        current /= component;
        std::error_code error; const auto status = fs::symlink_status(current,error);
        if (error && error != std::errc::no_such_file_or_directory) throw fs::filesystem_error("inspect package path",current,error);
        if (!fs::exists(status)) { if (!create) throw std::runtime_error("package: directory missing"); fs::create_directory(current); }
        if (!(attributes(current) & FILE_ATTRIBUTE_DIRECTORY)) throw std::runtime_error("package: expected ordinary directory");
    }
    return fs::canonical(absolute);
}
// 遍历前固定根，逐项拒绝重解析及非普通文件，并在读取正文前核对计量上限。
// 步骤：校验根和限制 → 有界遍历目录 → 核对实际边界和文件类型 → 汇总字节及相对路径。
std::vector<Item> inspect(const fs::path& root, Limits limits) {
    const auto base = directory(root);
    if (!limits.entries || !limits.depth) throw std::invalid_argument("package: positive tree limits required");
    std::vector<Item> items; std::size_t bytes = 0;
    for (auto it = fs::recursive_directory_iterator(base); it != fs::recursive_directory_iterator(); ++it) {
        if (items.size() == limits.entries || static_cast<std::size_t>(it.depth())+1 > limits.depth) throw std::length_error("package: tree limit exceeded");
        if (!portable(it->path().filename())) throw std::runtime_error("package: unsafe file name");
        const bool folder = (attributes(it->path()) & FILE_ATTRIBUTE_DIRECTORY) != 0; within(base,it->path());
        if (!folder) {
            if (!fs::is_regular_file(fs::symlink_status(it->path()))) throw std::runtime_error("package: ordinary files required");
            const auto length = fs::file_size(it->path());
            if (length > limits.bytes - bytes) throw std::length_error("package: byte limit exceeded");
            bytes += static_cast<std::size_t>(length);
        }
        items.push_back({it->path().lexically_relative(base),folder});
    }
    return items;
}
// 创建唯一私有名额，不复用既有暂存目录或把半包放入可发现名称。
// 步骤：核对根及私有前缀 → 有界生成候选身份 → 创建并复核直接子目录。
fs::path temporary(const fs::path& root, const std::string& prefix) {
    const auto base = directory(root,true);
    if (prefix.empty() || prefix.front() != '.') throw std::invalid_argument("package: private prefix required");
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        const auto name = prefix+std::to_string(GetCurrentProcessId())+"-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(runtime::next_domain());
        if (!portable(name)) throw std::invalid_argument("package: unsafe private prefix");
        const auto path = base/name;
        if (fs::create_directory(path)) return child(base,path);
    }
    throw std::runtime_error("package: cannot reserve private directory");
}
// 移动前重新解析两个边界，Windows 不覆盖目标的改名保持发布完整性。
// 步骤：确认来源直接归属 → 核对新目标及父目录 → 执行不覆盖的本地改名。
void move(const fs::path& root, const fs::path& source, const fs::path& destination) {
    const auto base = directory(root), from = child(base,source);
    const auto to = fs::absolute(destination).lexically_normal();
    if (directory(to.parent_path()) != base || !portable(to.filename()) || fs::exists(fs::symlink_status(to)))
        throw std::runtime_error("package: destination must be a fresh managed child");
    if (!MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("package: cannot move managed directory");
}
// 仅清理经过完整校验的私有树，每个实际删除前再次核对路径，不跟随外部链接。
// 步骤：核对私有直接子目录 → 完整检查树 → 逆序删除已校验子项 → 最后删除空私有目录。
void erase(const fs::path& root, const fs::path& target, Limits limits) {
    const auto base = directory(root), path = child(base,target);
    if (!path.filename().native().starts_with(L'.')) throw std::runtime_error("package: only private staging may be erased");
    const auto items = inspect(path,limits);
    for (auto item = items.rbegin(); item != items.rend(); ++item) {
        const auto current = path/item->relative; (void)directory(current.parent_path()); const auto flags = attributes(current); within(path,current);
        if (!item->directory && (flags & FILE_ATTRIBUTE_READONLY)) throw std::runtime_error("package: read-only file prevents cleanup");
        if (!fs::remove(current)) throw std::runtime_error("package: private item could not be removed");
    }
    if (child(base,path) != path || !fs::remove(path)) throw std::runtime_error("package: private directory could not be removed");
}
}
