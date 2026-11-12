#include <dafeiyu/plugin_module_v2.hpp>
#include <stdexcept>

namespace dfy::plugins {
namespace {
// 在执行 v2 DLL 代码前复核声明版本与当前包描述。
// 步骤：检查主 ABI → 重新读取清单 → 拒绝过期描述并返回入口。
std::filesystem::path checked_entry(const Manifest& manifest) {
    if (manifest.abi_version != DFY_V2_ABI_VERSION) throw std::runtime_error("plugin v2: unsupported manifest ABI");
    const auto current = load_manifest(manifest.directory);
    if (current != manifest) throw std::runtime_error("plugin v2: manifest changed; refresh catalog");
    return current.entry;
}
}

// 在模块及活动保护覆盖期间查询和复制引导表，失败由成员自动收尾。
// 步骤：复核并加载 → 取得代码与活动保护 → 查询并校验状态及布局 → 保存宿主副本。
PluginModuleV2::PluginModuleV2(const Manifest& manifest, std::size_t lease_capacity, std::size_t call_capacity)
    : module_(checked_entry(manifest), lease_capacity, call_capacity) {
    auto lease = module_.lease();
    auto call = lease.enter();
    const auto query = reinterpret_cast<dfy_v2_query_fn>(call.symbol(DFY_V2_QUERY_EXPORT));
    const dfy_v2_plugin_api* reported = nullptr;
    dfy_v2_error error{};
    const auto status = query(DFY_V2_ABI_VERSION, &reported, &error);

    if (error.length > DFY_V2_ERROR_CAPACITY) throw std::runtime_error("plugin v2: invalid error length");
    // 失败结果中的指针和 DLL 错误正文均不读取。
    if (status != DFY_V2_OK) throw std::runtime_error("plugin v2: failed status " + std::to_string(status));
    if (!reported || error.length) throw std::runtime_error("plugin v2: invalid success result");
    if (reported->struct_size != sizeof(dfy_v2_plugin_api)) throw std::runtime_error("plugin v2: incompatible table size");
    if (reported->abi_version != DFY_V2_ABI_VERSION) throw std::runtime_error("plugin v2: ABI version mismatch");
    if (reported->architecture != DFY_V2_ARCH_X64 || reported->reserved) {
        throw std::runtime_error("plugin v2: incompatible architecture or reserved field");
    }
    if (!reported->query_interface) throw std::runtime_error("plugin v2: missing required function");
    api_ = *reported;
}

// 仅在平台确实释放本次引用后清空引导表，拒绝或异常均保留可重试状态。
platform::ModuleClose PluginModuleV2::close() {
    const auto result = module_.close();
    if (result == platform::ModuleClose::closed) api_ = {};
    return result;
}
}
