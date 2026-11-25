#include "poll_log.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <system_error>

namespace dfy::management {
// 只保存固定类别、系统码及发生时间，异常文本留在原请求的回执。
// 步骤：分类 → 结算被替换的错误 → 累加样本 → 记录首次及每分钟的在途汇总。
void PollLog::failure(app::SoftwareEvent event, const std::exception& error) {
    if (!log_) return;
    using namespace std::chrono;
    const auto now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    app::SoftwareFields fields;
    fields.poll_failure = app::PollFailureKind::management;
    if (const auto* system = dynamic_cast<const std::system_error*>(&error)) {
        fields.poll_failure = app::PollFailureKind::transport;
        fields.system_error = system->code().value();
    } else if (dynamic_cast<const nlohmann::json::exception*>(&error)) fields.poll_failure = app::PollFailureKind::protocol;

    auto& current = active_[event];
    if (current.count && (current.poll_failure!=fields.poll_failure || current.system_error!=fields.system_error)) {
        log_->record(event,app::SoftwareResult::ended,current);
        current = {};
    }
    if (!current.count) {
        current = fields;
        current.first_utc_ms = now;
        current.count = 0;
    }
    const bool report = !*current.count || now/60000 != *current.last_utc_ms/60000;
    ++*current.count;
    current.last_utc_ms = now;
    if (report) log_->record(event,app::SoftwareResult::failed,current);
}

// 从本次成功读取确认恢复，并归还该入口的计数。
void PollLog::success(app::SoftwareEvent event) {
    if (auto previous = active_.extract(event); !previous.empty())
        log_->record(event,app::SoftwareResult::recovered,previous.mapped());
}

// 关闭观察者时保留每个未恢复错误的最终计数。
PollLog::~PollLog() {
    for (const auto& [event,fields] : active_) log_->record(event,app::SoftwareResult::ended,fields);
}
}
