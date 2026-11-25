#pragma once
#include "../software_log.hpp"
#include <map>

namespace dfy::management {
// 每个固定读取入口独立归并错误，观察者关闭时结算尚未恢复的记录。
class PollLog {
  public:
    // 借用当前进程的软件日志，测试及无日志调用方可传空。
    explicit PollLog(app::SoftwareLog* log) : log_(log) {}
    // 将仍在失败的各个入口以结束状态写出。
    ~PollLog();
    // 分类本次错误，计算同类连续次数并保留首末采样。
    void failure(app::SoftwareEvent event, const std::exception& error);
    // 本入口读取恢复后结算前一段失败，连续成功保持静默。
    void success(app::SoftwareEvent event);
  private:
    app::SoftwareLog* log_;
    std::map<app::SoftwareEvent,app::SoftwareFields> active_;
};
}
