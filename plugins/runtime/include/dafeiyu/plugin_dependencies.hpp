#pragma once
#include <dafeiyu/plugin_instance.hpp>
#include <dafeiyu/handle_registry.hpp>

namespace dfy::plugins {
class PluginDependencies;
enum class DependencyStop { refuse_dependents, linked };
struct DependencyShutdown { ShutdownResult result; std::shared_ptr<PluginInstance> blocked; std::size_t closed = 0; };
// 绑定具体消费者和提供方实例的代码凭证，重载同名包不会替换旧绑定。
class DependencyBinding {
  public:
    // 禁止复制凭证本体，调用方共享同一受预算约束的绑定视图。
    DependencyBinding(const DependencyBinding&) = delete;
    // 禁止赋值或移动已发布绑定。
    DependencyBinding& operator=(const DependencyBinding&) = delete;
    // 返回本次绑定的具体消费者实例，不根据包名重新解析。
    const std::shared_ptr<PluginInstance>& consumer() const noexcept { return consumer_; }
    // 返回本次绑定的具体提供方实例，旧视图继续保持其寿命。
    const std::shared_ptr<PluginInstance>& provider() const noexcept { return provider_; }
    // 在使用提供方代码前取得活动保护，调用表及地址的借用仍须由本绑定覆盖。
    platform::CodeCall enter() const { return code_.enter(); }
  private:
    friend class PluginDependencies;
    // 在绑定发布前取得提供方代码持有，失败不创建依赖边。
    DependencyBinding(std::shared_ptr<PluginInstance> consumer, std::shared_ptr<PluginInstance> provider)
        : consumer_(std::move(consumer)), provider_(std::move(provider)), code_(provider_->retain_code()) {}
    std::shared_ptr<PluginInstance> consumer_, provider_;
    platform::CodeLease code_;
};

// 同宿主线程的有界运行时依赖目录，不进行包版本求解或配置角色绑定。
class PluginDependencies {
  public:
    // 固定依赖名额，所有关系和重新绑定都独立计费。
    explicit PluginDependencies(std::size_t capacity);
    // 禁止复制目录及依赖释放责任。
    PluginDependencies(const PluginDependencies&) = delete;
    // 禁止赋值或隐式移动仍有绑定的目录。
    PluginDependencies& operator=(const PluginDependencies&) = delete;
    // 绑定两个运行实例，拒绝自依赖及环，失败不改变已有绑定。
    runtime::Handle bind(std::shared_ptr<PluginInstance> consumer, std::shared_ptr<PluginInstance> provider);
    // 先准备新提供方的独立绑定，旧绑定仍保留；实际切换后由宿主归还旧关系。
    runtime::Handle rebind(runtime::Handle old_binding, std::shared_ptr<PluginInstance> provider);
    // 返回保持实例和代码的绑定视图，释放目录关系不会撤销仍被使用的视图。
    std::shared_ptr<const DependencyBinding> get(runtime::Handle binding) const { return references_.get(binding).binding; }
    // 在实际使用结束后归还一份关系，旧视图继续阻止其提供方卸载。
    void release(runtime::Handle binding);
    // 默认有使用者则拒绝；显式联动时先停止依赖方，失败保留未完成关系，重复调用继续剩余部分。
    DependencyShutdown shutdown(const std::shared_ptr<PluginInstance>& provider, DependencyStop mode = DependencyStop::refuse_dependents);
    // 返回目录中仍未归还的绑定数，旧绑定在重新绑定后仍占名额。
    std::size_t size() const noexcept { return references_.size(); }
  private:
    struct Reference { std::shared_ptr<const DependencyBinding> binding; };
    // 检查提供方是否已经直接或间接依赖消费者，防止新边成环。
    bool cyclic(const PluginInstance* consumer, const PluginInstance* provider) const;
    // 在任何停止副作用前冻结受影响实例及依赖方优先的顺序。
    std::vector<std::shared_ptr<PluginInstance>> stop_plan(const std::shared_ptr<PluginInstance>& provider) const;
    runtime::HandleRegistry<Reference> references_;
    std::vector<runtime::Handle> slots_;
};
}
