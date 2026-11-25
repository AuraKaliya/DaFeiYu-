#include "software_log.hpp"
#include "software_log_files.hpp"
#include <windows.h>
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>

namespace dfy::app {
namespace {
using namespace std::chrono;
constexpr std::array roles{"host","window","terminal"};
constexpr std::array events{"process.started","process.exiting","host.listening","host.launch","client.connected",
    "client.disconnected","stop.submitted","stop.accepted","stop.reply","profile.claim","configuration.read",
    "window.closing","webview.released","profile.cleaned","window.destroyed","conversation.drain","plugins.drain",
    "plugin.load","plugin.stop","plugin.unloaded","plugin.catalog"};
constexpr std::array results{"started","ok","failed","accepted","busy","pending"};
constexpr std::size_t queue_limit = 128;

struct Entry {
    SoftwareEvent event;
    SoftwareResult result;
    nlohmann::json fields;
    std::int64_t utc_ms, elapsed_ns;
    std::uint64_t sequence, lost;
};

// 核对随机身份的十进制分组格式，使自由文本无法经诊断字段写盘。
bool numeric_groups(std::string_view value, unsigned count) {
    for (unsigned index=0; index<count; ++index) {
        const auto end = value.find('-');
        const auto part = value.substr(0,end);
        if (part.empty() || part.size()>10 || part.find_first_not_of("0123456789")!=part.npos) return false;
        if (end==value.npos) return index+1==count;
        value.remove_prefix(end+1);
    }
    return false;
}

// 仅复制当前事件可关联的结构化身份和数值，停止操作在同一后台实例内唯一。
// 步骤：核对本体与窗口身份 → 从原实例派生退出操作号 → 保存已有数值。
nlohmann::json diagnostic_fields(const SoftwareFields& fields) {
    auto row = nlohmann::json::object();
    if (numeric_groups(fields.instance,3)) {
        row["instance"] = fields.instance;
        if (fields.shutdown) row["operation"] = "stop-"+std::string(fields.instance);
    }
    if (fields.client.starts_with("client-") && numeric_groups(fields.client.substr(7),4)) row["client"] = fields.client;
    if (fields.system_error) row["systemError"] = *fields.system_error;
    if (fields.exit_code) row["exitCode"] = *fields.exit_code;
    if (fields.duration_ms) row["durationMs"] = *fields.duration_ms;
    if (fields.count) row["count"] = *fields.count;
    if (fields.session) row["session"] = fields.session;
    if (fields.turn) row["turn"] = fields.turn;
    if (fields.plugin_instance) row["pluginInstance"] = fields.plugin_instance;
    if (fields.plugin_code) row["pluginCode"] = *fields.plugin_code;
    return row;
}
}

struct SoftwareLog::State {
    const steady_clock::time_point origin = steady_clock::now();
    std::atomic<DWORD> fault{0};
    std::atomic_bool recorded{false};
    std::mutex mutex;
    std::condition_variable_any ready;
    std::deque<Entry> pending;
    std::string instance, client;
    bool shutting_down = false;
    std::uint64_t sequence = 0, lost = 0;
    std::jthread worker;

    // 保留首次错误并向进程诊断输出报告一次，业务入口继续按原结果收尾。
    void fail(DWORD code) noexcept {
        DWORD clear = 0;
        if (fault.compare_exchange_strong(clear,code)) std::fprintf(stderr,"Software log unavailable (error %lu).\n",code);
    }

    // 在专用线程独占新的日志文件，逐条写入并在退出前排空队列。
    // 步骤：创建进程专属文件 → 等待事件或停止 → 编码允许字段并写出。
    void write(std::stop_token stop, const std::filesystem::path& profile, SoftwareRole role) {
        SoftwareLogFiles files(profile/"logs");
        const auto pid = GetCurrentProcessId();

        for (;;) {
            std::unique_lock lock(mutex);
            ready.wait(lock,stop,[this] { return !pending.empty(); });
            if (pending.empty()) break;
            auto entry = std::move(pending.front());
            pending.pop_front();
            lock.unlock();

            nlohmann::json row{{"schema",1},{"utcMs",entry.utc_ms},{"elapsedNs",entry.elapsed_ns},{"seq",entry.sequence},
                {"level",entry.result == SoftwareResult::failed ? "error" : "info"},{"component",roles.at(static_cast<std::size_t>(role))},
                {"pid",pid},{"event",events.at(static_cast<std::size_t>(entry.event))},{"result",results.at(static_cast<std::size_t>(entry.result))}};
            row.update(entry.fields);
            if (entry.lost) row["droppedBefore"] = entry.lost;
            if (entry.event == SoftwareEvent::process_started) {
                row["build"] = DFY_APPLICATION_BUILD;
                if (std::string_view(DFY_APPLICATION_COMMIT).size()) row["commit"] = DFY_APPLICATION_COMMIT;
            }
            files.append(row.dump()+"\n");
            recorded = true;
        }
        files.close();
    }
};

// 在日志线程创建文件，初始化失败仍允许产品按原入口运行。
// 步骤：分配进程内状态 → 启动独立写入 → 接纳启动记录。
SoftwareLog::SoftwareLog(const std::filesystem::path& profile, SoftwareRole role) noexcept {
    try {
        state_ = std::make_unique<State>();
        auto* state = state_.get();
        state->worker = std::jthread([state,profile,role](std::stop_token stop) {
            try { state->write(stop,profile,role); }
            catch (const std::system_error& error) { state->fail(error.code().value()); }
            catch (...) { state->fail(ERROR_GEN_FAILURE); }
        });
        record(SoftwareEvent::process_started,SoftwareResult::started);
    } catch (...) {
        if (state_) state_->fail(ERROR_NOT_ENOUGH_MEMORY);
        else std::fputs("Software log unavailable (out of memory).\n",stderr);
    }
}

// jthread 先通知停止并等待已有记录实际写完，再释放队列和同步对象。
SoftwareLog::~SoftwareLog() = default;

// 在身份已经得到握手或宿主创建确认后更新默认关联，旧队列中的记录保持原值。
void SoftwareLog::bind(std::string_view instance, std::string_view client) noexcept {
    if (error() || !numeric_groups(instance,3) || (!client.empty() &&
        !(client.starts_with("client-") && numeric_groups(client.substr(7),4)))) return;
    try {
        std::lock_guard lock(state_->mutex);
        if (instance!=state_->instance) state_->shutting_down = false;
        state_->instance = instance;
        state_->client = client;
    } catch (...) { state_->fail(ERROR_NOT_ENOUGH_MEMORY); }
}

// 以进程内顺序签收有界记录，保留退出槽位和已丢失事件的数量。
// 步骤：核对写入状态与容量 → 在同一锁内采时和编号 → 唤醒写入线程。
void SoftwareLog::record(SoftwareEvent event, SoftwareResult result, SoftwareFields fields) noexcept {
    if (error()) return;
    try {
        std::lock_guard lock(state_->mutex);
        const auto limit = event == SoftwareEvent::process_exiting ? queue_limit : queue_limit-1;
        if (state_->pending.size() >= limit) { ++state_->lost; return; }
        if (fields.instance.empty()) fields.instance = state_->instance;
        if (fields.client.empty() && fields.instance==state_->instance) fields.client = state_->client;
        if (!state_->instance.empty() && fields.instance==state_->instance) {
            if (fields.shutdown && result==SoftwareResult::accepted && (event==SoftwareEvent::stop_accepted || event==SoftwareEvent::stop_reply))
                state_->shutting_down = true;
            fields.shutdown |= state_->shutting_down;
        }
        state_->pending.push_back({event,result,diagnostic_fields(fields),duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count(),
            duration_cast<nanoseconds>(steady_clock::now()-state_->origin).count(),++state_->sequence,state_->lost});
        state_->lost = 0;
        state_->ready.notify_one();
    } catch (...) { state_->fail(ERROR_NOT_ENOUGH_MEMORY); }
}

// 在进程返回前登记预期退出码，调用方沿用原来的结果。
int SoftwareLog::exiting(int code) noexcept {
    record(SoftwareEvent::process_exiting,code ? SoftwareResult::failed : SoftwareResult::ok,{.exit_code=code});
    return code;
}

// 不接触文件系统即可取得当前日志错误状态。
unsigned long SoftwareLog::error() const noexcept { return state_ ? state_->fault.load() : ERROR_NOT_ENOUGH_MEMORY; }

// 只按写入线程已经刷新的实效报告日志可用。
bool SoftwareLog::recorded() const noexcept { return state_ && state_->recorded.load(); }
}
