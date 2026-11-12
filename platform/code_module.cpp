#include <dafeiyu/code_module.hpp>
#include <stdexcept>

namespace dfy::platform {
// 校验配置后加载模块，避免零容量配置也触发 DLL 初始化。
// 步骤：拒绝零容量 → 建立共享平台持有及代码容量。
CodeModule::CodeModule(const std::filesystem::path& file, std::size_t lease_capacity) {
    if (!lease_capacity) throw std::invalid_argument("module: positive lease capacity required");
    storage_ = std::make_shared<Storage>(file, lease_capacity);
}

// 创建独立代码凭证，关闭状态及容量统一由凭证入口校验。
CodeLease CodeModule::lease() { return CodeLease(storage_); }

// 仅在无代码依赖时显式释放平台引用，失败不提前记录关闭成功。
// 步骤：复用已关闭结果 → 拒绝仍被持有的模块 → 释放平台引用并确认关闭。
ModuleClose CodeModule::close() {
    if (storage_->closed) return ModuleClose::closed;
    if (storage_->leases) return ModuleClose::in_use;
    storage_->library.close();
    storage_->closed = true;
    return ModuleClose::closed;
}

// 取得共享模块的独立代码持有，失败不增加凭证计量。
// 步骤：核对模块存活 → 核对容量 → 登记本次代码持有。
CodeLease::CodeLease(std::shared_ptr<CodeModule::Storage> storage) : storage_(std::move(storage)) {
    if (!storage_ || storage_->closed) throw std::logic_error("module: no live code");
    if (storage_->leases == storage_->capacity) throw std::length_error("module: lease capacity exhausted");
    ++storage_->leases;
}

// 显式产生另一份独立代码凭证，不依赖控制对象继续存活。
CodeLease CodeLease::retain() const { return CodeLease(storage_); }

// 在凭证覆盖的模块内查找函数地址，不延长返回地址自身的寿命。
DynamicLibrary::Symbol CodeLease::symbol(const std::string& name) const {
    if (!storage_) throw std::logic_error("module: empty code lease");
    return storage_->library.symbol(name);
}

// 移动接收代码持有，释放原有责任且不重复计费。
// 步骤：处理自移动 → 归还旧持有 → 转交来源持有。
CodeLease& CodeLease::operator=(CodeLease&& other) noexcept {
    if (this == &other) return *this;
    reset();
    storage_ = std::move(other.storage_);
    return *this;
}

// 释放一次代码持有，控制对象已退出时由最后持有者触发平台析构。
void CodeLease::reset() noexcept {
    if (!storage_) return;
    --storage_->leases;
    storage_.reset();
}
}
