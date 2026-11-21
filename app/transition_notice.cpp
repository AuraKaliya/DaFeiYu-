#include "transition_notice.hpp"
#include "run_log.hpp"
#include <nlohmann/json.hpp>

namespace dfy::app {
// 派送已发生的系统事实并单独记录观察结果。
// 步骤：投影只读身份 → 同步通知适用处理器 → 记录观察异常。
void notify_transition(plugins::PluginManager& plugins, RunLog* log, const TransitionNotice& notice) noexcept {
    try {
        const auto result = plugins.transition_hook({notice.point,0,notice.status,0,notice.owner,
            notice.origin.domain,notice.origin.sequence,notice.ask,notice.message.domain,notice.message.sequence,
            {notice.category.data(),notice.category.size()},{}});
        if (log && result.observed) log->append("hook.observed",{{"point",notice.point},{"domain",notice.origin.domain},
            {"sequence",notice.origin.sequence},{"ask",notice.ask},{"version",result.version},{"errors",result.errors}});
    } catch (...) {}
}
}
