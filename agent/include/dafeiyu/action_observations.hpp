#pragma once
#include <dafeiyu/observation_inbox.hpp>

namespace dfy::agent {
// 原行动观察集合的不可变绑定；身份只关联宿主采样，不自行授予工具权限。
class ActionObservations {
  public:
    // 固定一至四个互异对象的原集合检查，复制本对象保持同一绑定身份。
    explicit ActionObservations(std::vector<ObservationGuard> guards);
    // 返回进程内签发的集合身份，不包含原生地址或模型可生成的授权材料。
    runtime::DomainId binding() const noexcept { return binding_; }
    // 只读检查全部原对象当前性，不重采样或修改执行状态。
    bool current(ObservationTime now) const noexcept;
    // 判断是否完整更新原集合的相同对象及 schema，要求全部采样修订增加且不早于宿主请求。
    bool succeeds(const ActionObservations& previous, ObservationTime now, ObservationTime not_before) const noexcept;
    // 复制原有计费记录，供模型输入与动作前置使用完全相同的观察。
    std::vector<std::shared_ptr<const ObservationRecord>> observations() const;
  private:
    runtime::DomainId binding_;
    std::vector<ObservationGuard> guards_;
};
}
