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
constexpr std::array events{"process.started","process.exiting"};
constexpr std::array results{"started","ok","failed"};
constexpr std::size_t queue_limit = 128;

struct Entry {
    SoftwareEvent event;
    SoftwareResult result;
    std::optional<int> exit_code;
    std::int64_t utc_ms, elapsed_ns;
    std::uint64_t sequence, lost;
};

}

struct SoftwareLog::State {
    const steady_clock::time_point origin = steady_clock::now();
    std::atomic<DWORD> fault{0};
    std::atomic_bool recorded{false};
    std::mutex mutex;
    std::condition_variable_any ready;
    std::deque<Entry> pending;
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
            const auto entry = pending.front();
            pending.pop_front();
            lock.unlock();

            nlohmann::json row{{"schema",1},{"utcMs",entry.utc_ms},{"elapsedNs",entry.elapsed_ns},{"seq",entry.sequence},
                {"level",entry.result == SoftwareResult::failed ? "error" : "info"},{"component",roles.at(static_cast<std::size_t>(role))},
                {"pid",pid},{"event",events.at(static_cast<std::size_t>(entry.event))},{"result",results.at(static_cast<std::size_t>(entry.result))}};
            if (entry.lost) row["droppedBefore"] = entry.lost;
            if (entry.exit_code) row["exitCode"] = *entry.exit_code;
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

// 以进程内顺序签收有界记录，保留退出槽位和已丢失事件的数量。
// 步骤：核对写入状态与容量 → 在同一锁内采时和编号 → 唤醒写入线程。
void SoftwareLog::record(SoftwareEvent event, SoftwareResult result, std::optional<int> exit_code) noexcept {
    if (error()) return;
    try {
        std::lock_guard lock(state_->mutex);
        const auto limit = event == SoftwareEvent::process_exiting ? queue_limit : queue_limit-1;
        if (state_->pending.size() >= limit) { ++state_->lost; return; }
        state_->pending.push_back({event,result,exit_code,duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count(),
            duration_cast<nanoseconds>(steady_clock::now()-state_->origin).count(),++state_->sequence,state_->lost});
        state_->lost = 0;
        state_->ready.notify_one();
    } catch (...) { state_->fail(ERROR_NOT_ENOUGH_MEMORY); }
}

// 在进程返回前登记预期退出码，调用方沿用原来的结果。
int SoftwareLog::exiting(int code) noexcept {
    record(SoftwareEvent::process_exiting,code ? SoftwareResult::failed : SoftwareResult::ok,code);
    return code;
}

// 不接触文件系统即可取得当前日志错误状态。
unsigned long SoftwareLog::error() const noexcept { return state_ ? state_->fault.load() : ERROR_NOT_ENOUGH_MEMORY; }

// 只按写入线程已经刷新的实效报告日志可用。
bool SoftwareLog::recorded() const noexcept { return state_ && state_->recorded.load(); }
}
