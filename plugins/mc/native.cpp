#include "pipe_exchange.hpp"
#include "inspect_work.hpp"
#include "world_protocol.hpp"
#include <dafeiyu/tool_abi_v2.h>
#include <dafeiyu/companion_abi_v2.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>

struct NativeJob {
    std::uint64_t sequence = 0;
    bool ready = false, success = false, exited = false;
    std::string id, output, wire, arguments;
    bool inbox = false;
    bool cancelled = false;
    std::uint32_t effect = DFY_V2_TOOL_NOT_APPLIED;
    std::unique_ptr<dfy::mc::InspectWork> inspect;
    std::unique_ptr<dfy::mc::PipeExchange> pipe;
};
struct dfy_v2_instance {
    std::uint64_t owner = 0, sequence = 0;
    bool running = false;
    dfy::mc::BridgeConfig config;
    std::array<NativeJob, 4> jobs;
};

namespace {
// 每个实例只能在原宿主线程上操作，已关闭槽位不再持有 DLL 内对象。
thread_local std::array<dfy_v2_instance*, 16> slots{};
std::atomic_uint64_t serial{0};

// 限长读取 ABI 借用文本，非法空指针不形成视图。
std::string_view text(dfy_v2_string_view value) noexcept {
    if (!value.data || value.length > 65536) return {};
    return {value.data, static_cast<std::size_t>(value.length)};
}
// 向宿主借出由当前实例或 DLL 拥有的文本。
dfy_v2_string_view view(std::string_view value) noexcept { return {value.data(), value.size()}; }
// 清空可选错误正文并返回稳定错误类别。
dfy_v2_status report(dfy_v2_status status, dfy_v2_error* error) noexcept {
    if (error) error->length = 0;
    return status;
}
// 只在本线程的活动实例集合中解析原始地址。
bool owned(const dfy_v2_instance* raw) noexcept {
    return raw && std::find(slots.begin(), slots.end(), raw) != slots.end();
}
// 对照具体实例和工作序号核对原票据。
NativeJob* find_job(dfy_v2_instance* instance, dfy_v2_tool_ticket ticket) noexcept {
    if (!owned(instance) || !ticket.sequence || instance->owner != ticket.owner) return nullptr;
    for (auto& job : instance->jobs) if (job.sequence == ticket.sequence) return &job;
    return nullptr;
}
// 只有所有原生票据都归还后才允许停服或销毁。
bool active_jobs(const dfy_v2_instance& instance) noexcept {
    return std::any_of(instance.jobs.begin(), instance.jobs.end(), [](const auto& job) { return job.sequence != 0; });
}
// 只接收无参数对象，允许四种 JSON 空白。
bool empty_args(std::string_view args) noexcept {
    constexpr std::string_view spaces = " \r\n\t";
    const auto first = args.find_first_not_of(spaces), last = args.find_last_not_of(spaces);
    return first != args.npos && last > first && args[first] == '{' && args[last] == '}' &&
        args.substr(first + 1, last - first - 1).find_first_not_of(spaces) == args.npos;
}

// 从插件独立配置构造可撤销通道，失败时不发布半成实例。
// 步骤：预留本线程槽位 → 校验配置及签发身份 → 移交实例。
dfy_v2_status DFY_V2_CALL create(dfy_v2_query_interface_fn host, dfy_v2_string_view config,
    dfy_v2_instance** output, dfy_v2_error* error) noexcept {
    if (!output) return report(DFY_V2_INVALID_ARGUMENT, error);
    *output = nullptr;
    if (!host) return report(DFY_V2_INVALID_ARGUMENT, error);
    const auto free = std::find(slots.begin(), slots.end(), nullptr);
    if (free == slots.end()) return report(DFY_V2_RESOURCE_EXHAUSTED, error);
    try {
        auto instance = std::make_unique<dfy_v2_instance>();
        instance->config = dfy::mc::configured_bridge(text(config));
        auto previous = serial.load();
        do {
            if (previous == UINT64_MAX) return report(DFY_V2_RESOURCE_EXHAUSTED, error);
        } while (!serial.compare_exchange_weak(previous, previous + 1));
        instance->owner = previous + 1;
        *free = *output = instance.release();
        return report(DFY_V2_OK, error);
    } catch (const std::bad_alloc&) { return report(DFY_V2_RESOURCE_EXHAUSTED, error); }
    catch (...) { return report(DFY_V2_INVALID_ARGUMENT, error); }
}
// 开启当前实例的工具接纳。
dfy_v2_status DFY_V2_CALL start(dfy_v2_instance* raw, dfy_v2_error* error) noexcept {
    if (!owned(raw)) return report(DFY_V2_INVALID_HANDLE, error);
    raw->running = true;
    return report(DFY_V2_OK, error);
}
// 等待工作票据释放后关闭接纳。
dfy_v2_status DFY_V2_CALL stop(dfy_v2_instance* raw, dfy_v2_error* error) noexcept {
    if (!owned(raw)) return report(DFY_V2_INVALID_HANDLE, error);
    if (active_jobs(*raw)) return report(DFY_V2_BUSY, error);
    raw->running = false;
    return report(DFY_V2_OK, error);
}
// 读取实例停止状态，输出失败时归零。
dfy_v2_status DFY_V2_CALL stopped(dfy_v2_instance* raw, uint32_t* value, dfy_v2_error* error) noexcept {
    if (!value) return report(DFY_V2_INVALID_ARGUMENT, error);
    *value = 0;
    if (!owned(raw)) return report(DFY_V2_INVALID_HANDLE, error);
    *value = raw->running ? DFY_V2_STOP_PENDING : DFY_V2_STOPPED;
    return report(DFY_V2_OK, error);
}
// 只销毁实际停止且无剩余票据的原实例。
void DFY_V2_CALL destroy(dfy_v2_instance* raw) noexcept {
    if (!owned(raw) || raw->running || active_jobs(*raw)) return;
    *std::find(slots.begin(), slots.end(), raw) = nullptr;
    delete raw;
}
// 活动插件按配对状态公开世界与私有收件读口。
dfy_v2_status DFY_V2_CALL count(dfy_v2_instance* raw, uint32_t* value, dfy_v2_error* error) noexcept {
    if (!value) return report(DFY_V2_INVALID_ARGUMENT, error);
    *value = owned(raw) && raw->running ? (raw->config.key.empty() ? 1u : 3u) : 0u;
    return report(*value ? DFY_V2_OK : DFY_V2_INVALID_CONTEXT, error);
}
// 返回世界状态的无参契约，工具描述与连接寿命无关。
dfy_v2_status DFY_V2_CALL describe(dfy_v2_instance* raw, uint32_t index, dfy_v2_tool_description* out, dfy_v2_error* error) noexcept {
    if (!out) return report(DFY_V2_INVALID_ARGUMENT, error);
    if (out->struct_size != sizeof(*out)) { out->struct_size = 0; return report(DFY_V2_ABI_MISMATCH, error); }
    *out = {};
    if (!owned(raw) || !raw->running) return report(DFY_V2_INVALID_CONTEXT, error);
    if (!index) {
        *out = {sizeof(*out), 1, 0, 0, view("world"), view("Read the current companion world and position."),
            view(R"({"type":"object","properties":{},"additionalProperties":false})")};
    } else if (index == 1 && !raw->config.key.empty()) {
        *out = {sizeof(*out), 1, 0, 0, view("inbox"), view("Read messages from the bound player."),
            view(R"({"type":"object","properties":{"generation":{"type":"string"},"after":{"type":"integer"}},"required":["generation","after"],"additionalProperties":false})")};
    } else if (index == 2 && !raw->config.key.empty()) {
        *out = {sizeof(*out), 1, DFY_V2_TOOL_CHANGES_STATE, 0, view("inspect"), view("Visit the landmark identified by the player's original inspection order."),
            view(R"({"type":"object","properties":{"generation":{"type":"string"},"order":{"type":"integer"},"place":{"type":"string"}},"required":["generation","order","place"],"additionalProperties":false})")};
    } else return report(DFY_V2_NOT_FOUND, error);
    return report(DFY_V2_OK, error);
}
// 将只读请求和独占移动分配到有界工作槽，接纳本身不执行游戏动作。
// 步骤：校验能力与参数 → 核对移动互斥并分配独立槽位 → 签发工作票据。
dfy_v2_status DFY_V2_CALL begin(dfy_v2_instance* raw, dfy_v2_string_view name, const dfy_v2_tool_call* call,
    dfy_v2_tool_ticket* ticket, dfy_v2_error* error) noexcept {
    if (!ticket) return report(DFY_V2_INVALID_ARGUMENT, error);
    *ticket = {};
    if (!owned(raw) || !raw->running) return report(DFY_V2_INVALID_CONTEXT, error);
    const bool inbox = text(name) == "inbox" && !raw->config.key.empty();
    const bool inspect = text(name) == "inspect" && !raw->config.key.empty();
    if (!inbox && !inspect && text(name) != "world") return report(DFY_V2_NOT_FOUND, error);
    if (!call || call->struct_size != sizeof(*call) || call->tool_version != 1) return report(DFY_V2_ABI_MISMATCH, error);
    if (!call->session || !call->run || !call->step || !call->attempt || !call->invocation ||
        text(call->call_id).empty() || call->call_id.length > 128 || (!inbox && !inspect && !empty_args(text(call->arguments))))
        return report(DFY_V2_INVALID_ARGUMENT, error);
    if (inspect && std::any_of(raw->jobs.begin(), raw->jobs.end(), [](const auto& job) { return job.sequence && job.inspect; }))
        return report(DFY_V2_BUSY, error);
    const auto found = std::find_if(raw->jobs.begin(), raw->jobs.end(), [](const auto& job) { return job.sequence == 0; });
    if (found == raw->jobs.end()) return report(DFY_V2_BUSY, error);
    auto& job = *found;
    if (raw->sequence == UINT64_MAX) return report(DFY_V2_RESOURCE_EXHAUSTED, error);
    try {
        job.id = std::to_string(raw->owner) + "." + std::to_string(raw->sequence + 1);
        job.arguments = text(call->arguments);
        if (inspect) job.inspect = std::make_unique<dfy::mc::InspectWork>(raw->config, job.id, job.arguments);
        else job.wire = inbox ? dfy::mc::inbox_request(job.id, raw->config.key, job.arguments) : dfy::mc::world_request(job.id);
    } catch (const std::bad_alloc&) { return report(DFY_V2_RESOURCE_EXHAUSTED, error); }
    catch (...) { return report(DFY_V2_INVALID_ARGUMENT, error); }
    job.inbox = inbox;
    job.sequence = ++raw->sequence;
    *ticket = {raw->owner, job.sequence};
    return report(DFY_V2_OK, error);
}
// 非等待推进原查询，将消息校验和资源实际退出分开报告。
// 步骤：核对票据 → 打开并推进管道或进入退出阶段 → 复制稳定的结果值。
dfy_v2_status DFY_V2_CALL poll(dfy_v2_instance* raw, dfy_v2_tool_ticket ticket, dfy_v2_tool_progress* out,
    dfy_v2_error* error) noexcept {
    if (!out) return report(DFY_V2_INVALID_ARGUMENT, error);
    if (out->struct_size != sizeof(*out)) { out->struct_size = 0; return report(DFY_V2_ABI_MISMATCH, error); }
    *out = {};
    auto* job = find_job(raw, ticket);
    if (!job) return report(DFY_V2_INVALID_HANDLE, error);
    if (job->ready) {
        job->pipe.reset();
        job->exited = true;
    } else if (job->inspect) {
        try {
            if (auto value = job->inspect->poll(job->cancelled)) {
                job->output = std::move(value->payload);
                job->effect = value->moved ? DFY_V2_TOOL_APPLIED : DFY_V2_TOOL_NOT_APPLIED;
                job->success = value->arrived && !job->cancelled;
                job->ready = true;
            }
        } catch (...) {
            job->effect = job->inspect->attempted() ? DFY_V2_TOOL_UNKNOWN : DFY_V2_TOOL_NOT_APPLIED;
            job->ready = true;
        }
    } else {
        try {
            if (!job->pipe) job->pipe = std::make_unique<dfy::mc::PipeExchange>(raw->config.channel, job->wire);
            if (auto reply = job->pipe->poll()) {
                job->output = job->inbox ? dfy::mc::inbox_reply(*reply, job->id, job->arguments) : dfy::mc::world_reply(*reply, job->id);
                job->success = job->ready = true;
            }
        } catch (...) { job->ready = true; }
    }
    *out = {sizeof(*out), job->ready, job->success, job->effect, job->exited, 0, view(job->output)};
    return report(DFY_V2_OK, error);
}
// 取消尚未交付结果的世界查询，原票据继续负责退出。
dfy_v2_status DFY_V2_CALL cancel(dfy_v2_instance* raw, dfy_v2_tool_ticket ticket, uint32_t reason, dfy_v2_error* error) noexcept {
    auto* job = find_job(raw, ticket);
    if (!job) return report(DFY_V2_INVALID_HANDLE, error);
    if (reason < DFY_V2_CANCEL_USER || reason > DFY_V2_CANCEL_DEADLINE) return report(DFY_V2_INVALID_ARGUMENT, error);
    if (!job->ready) {
        job->cancelled = true;
        if (!job->inspect) job->ready = true;
    }
    return report(DFY_V2_OK, error);
}
// 归还已退出的票据，未退出工作保持占用。
dfy_v2_status DFY_V2_CALL release(dfy_v2_instance* raw, dfy_v2_tool_ticket ticket, dfy_v2_error* error) noexcept {
    auto* job = find_job(raw, ticket);
    if (!job) return report(DFY_V2_INVALID_HANDLE, error);
    if (!job->exited) return report(DFY_V2_BUSY, error);
    *job = {};
    return report(DFY_V2_OK, error);
}
const dfy_v2_lifecycle_api lifecycle{{sizeof(dfy_v2_lifecycle_api), 1}, create, start, stop, stopped, destroy};
const dfy_v2_tool_api tools{{sizeof(dfy_v2_tool_api), 1}, count, describe, begin, poll, cancel, release};
// 在活动实例内声明游戏陪伴的只读查询入口。
dfy_v2_status DFY_V2_CALL companion_info(dfy_v2_instance* raw, dfy_v2_companion_description* out, dfy_v2_error* error) noexcept {
    if (!out) return report(DFY_V2_INVALID_ARGUMENT, error);
    if (out->struct_size != sizeof(*out)) { out->struct_size = 0; return report(DFY_V2_ABI_MISMATCH, error); }
    *out = {};
    if (!owned(raw) || !raw->running) return report(DFY_V2_INVALID_CONTEXT, error);
    *out = {sizeof(*out), 0, view("world"), view("world")};
    return report(DFY_V2_OK, error);
}
const dfy_v2_companion_api companion{{sizeof(dfy_v2_companion_api), 1}, companion_info};
// 返回固定版本的静态能力表。
dfy_v2_status DFY_V2_CALL query(dfy_v2_string_view name, uint32_t version, const void** out, dfy_v2_error* error) noexcept {
    if (!out) return report(DFY_V2_INVALID_ARGUMENT, error);
    *out = nullptr;
    if (version == 1 && text(name) == DFY_V2_LIFECYCLE_INTERFACE) *out = &lifecycle;
    if (version == 1 && text(name) == DFY_V2_TOOL_INTERFACE) *out = &tools;
    if (version == DFY_V2_COMPANION_VERSION && text(name) == DFY_V2_COMPANION_INTERFACE) *out = &companion;
    return report(*out ? DFY_V2_OK : DFY_V2_UNSUPPORTED, error);
}
const dfy_v2_plugin_api plugin{sizeof(dfy_v2_plugin_api), DFY_V2_ABI_VERSION, DFY_V2_ARCH_X64, 0, query};
}

// 发布独立 MC 插件的 v2 引导表。
DFY_V2_EXPORT dfy_v2_status DFY_V2_CALL dfy_plugin_query_v2(uint32_t version, const dfy_v2_plugin_api** out, dfy_v2_error* error) {
    if (!out) return report(DFY_V2_INVALID_ARGUMENT, error);
    *out = version == DFY_V2_ABI_VERSION ? &plugin : nullptr;
    return report(*out ? DFY_V2_OK : DFY_V2_ABI_MISMATCH, error);
}
