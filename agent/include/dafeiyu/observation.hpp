#pragma once
#include <dafeiyu/runtime_ids.hpp>
#include <dafeiyu/struct_value.hpp>
#include <chrono>

namespace dfy::agent {
using ObservationTime = std::chrono::steady_clock::time_point;
enum class ObservationTrust { external_data, host_verified };
enum class ObservationValidity { fresh, future, expired, source_revoked };
struct ObservationObject { std::string id; std::uint64_t generation = 0; };
// 来源和信任由宿主签发，载荷字段不能覆盖这些控制元数据。
struct ObservationMetadata {
    runtime::DomainId source = 0, provider_instance = 0;
    ObservationTrust trust = ObservationTrust::external_data;
    ObservationObject object;
    // 来源对该对象签发的采样修订；即使内容相同，新的采样也应使用更高修订。
    std::uint64_t revision = 0;
    ObservationTime sampled, expires;
};
struct ObservationBinding;
class ObservationSource;
// 不可变观察值；载荷可继续读取，但时间有效不代表已核对对象的最新修订。
class ObservationSnapshot {
  public:
    // 复制不可变观察持有，不转移或清空原值。
    ObservationSnapshot(const ObservationSnapshot&) = default;
    // 替换本地视图，不修改任何已有观察值或来源授权。
    ObservationSnapshot& operator=(const ObservationSnapshot&) = default;
    // 借用固定元数据，调用方不能修改来源、对象代次或原修订。
    const ObservationMetadata& metadata() const noexcept;
    // 共享宿主不可变结构值，不持有原生函数、DLL 或生产工具凭证。
    std::shared_ptr<const runtime::StructValue> payload() const noexcept;
    // 在原宿主线程核对来源及时间边界，不调度观察、模型或动作。
    ObservationValidity validity(ObservationTime now) const noexcept;
  private:
    friend class ObservationSource;
    struct State;
    // 接收完整准备的不可变值和来源撤销状态，不拥有来源授权生命周期。
    explicit ObservationSnapshot(std::shared_ptr<const State> state) : state_(std::move(state)) {}
    std::shared_ptr<const State> state_;
};
// 同宿主线程的观察来源；销毁即撤销旧观察的新鲜资格，不销毁外部保留的数据值。
class ObservationSource {
  public:
    // 为宿主指定的提供实例及信任等级签发独立来源身份。
    ObservationSource(runtime::DomainId provider_instance, ObservationTrust trust);
    // 撤销来源，不执行插件停止或丢弃外部仍持有的历史观察。
    ~ObservationSource();
    // 禁止复制来源撤销责任。
    ObservationSource(const ObservationSource&) = delete;
    // 禁止赋值或隐式移动覆盖正在使用的来源。
    ObservationSource& operator=(const ObservationSource&) = delete;
    // 返回宿主签发的来源身份，不取自观察正文。
    runtime::DomainId id() const noexcept;
    // 构造一次观察：载荷最多 64 KiB／1024 节点、有效期至多一小时；不负责对象修订去重。
    ObservationSnapshot capture(ObservationObject object, std::uint64_t revision, ObservationTime sampled,
        ObservationTime expires, std::shared_ptr<const runtime::StructValue> payload) const;
    // 撤销旧来源的可用性；重复撤销无副作用，不能恢复已撤销来源。
    void revoke() noexcept;
  private:
    std::shared_ptr<ObservationBinding> binding_;
};
}
