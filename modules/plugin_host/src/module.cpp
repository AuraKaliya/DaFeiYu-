#include <dafeiyu/plugin_module.hpp>
#include <stdexcept>

namespace dfy::plugins {
namespace {
// 在执行 DLL 代码前复核清单身份、路径及宿主支持的 ABI。
// 步骤：拒绝不支持的声明版本 → 重新校验目录包 → 核对描述未变并返回入口。
std::filesystem::path checked_entry(const Manifest& manifest) {
    if (manifest.abi_version != DFY_ABI_VERSION) {
        throw std::runtime_error("plugin query: unsupported manifest ABI");
    }
    const auto current = load_manifest(manifest.directory);
    if (current != manifest) {
        throw std::runtime_error("plugin query: manifest changed; refresh catalog");
    }
    return current.entry;
}
}

// 查询并校验完整 v1 函数表，构造失败时由已构造的平台成员回收引用。
// 步骤：复核并加载入口 → 调用固定查询导出 → 检查返回状态和布局 → 保存已校验接口副本。
PluginModule::PluginModule(const Manifest& manifest) : library_(checked_entry(manifest)) {
    const auto query = reinterpret_cast<dfy_query_fn>(library_.symbol(DFY_QUERY_EXPORT));
    const dfy_plugin_api* reported = nullptr;
    dfy_error error{};
    const auto status = query(DFY_ABI_VERSION, &reported, &error);

    if (error.length > DFY_ERROR_CAPACITY) {
        throw std::runtime_error("plugin query: invalid error length");
    }
    // 不回显 DLL 的错误正文，也不解引用失败查询返回的任何表指针。
    if (status != DFY_OK) {
        throw std::runtime_error("plugin query: failed status " + std::to_string(status));
    }
    if (!reported || error.length != 0) {
        throw std::runtime_error("plugin query: invalid success result");
    }
    // 先检查固定前缀中的大小，不能在确认布局前复制整个表或访问尾部函数。
    if (reported->struct_size != sizeof(dfy_plugin_api)) {
        throw std::runtime_error("plugin query: incompatible table size");
    }
    if (reported->abi_version != manifest.abi_version) {
        throw std::runtime_error("plugin query: ABI version mismatch");
    }
    if (reported->architecture != DFY_ARCH_X64 || reported->reserved != 0) {
        throw std::runtime_error("plugin query: incompatible architecture or reserved field");
    }
    if (!reported->create || !reported->start || !reported->stop || !reported->destroy) {
        throw std::runtime_error("plugin query: missing required function");
    }

    api_ = *reported;
}

// 仅向仍持有模块的调用方提供接口，关闭后明确拒绝访问。
const dfy_plugin_api& PluginModule::api() const {
    if (!api_.create) {
        throw std::runtime_error("plugin query: module closed");
    }
    return api_;
}

// 成功释放平台引用后清空接口，释放失败时保留原状态。
void PluginModule::close() {
    library_.close();
    api_ = {};
}
}
