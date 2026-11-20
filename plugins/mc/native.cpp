#include "pipe_exchange.hpp"
#include "game_work.hpp"
#include "world_protocol.hpp"
#include "partner_loop.hpp"
#include <dafeiyu/activity_abi_v2.h>
#include <dafeiyu/hook_abi_v2.h>
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
    enum class Method { world, survey, inbox, ask, admit, receipt, control } method = Method::world;
    std::chrono::steady_clock::time_point next{};
    bool attempted = false;
    bool cancelled = false;
    std::uint32_t effect = DFY_V2_TOOL_NOT_APPLIED;
    std::uint64_t partner_session = 0, invocation = 0;
    std::string_view skill;
    std::unique_ptr<dfy::mc::GameWork> action;
    std::unique_ptr<dfy::mc::PipeExchange> pipe;
};
struct dfy_v2_instance {
    std::uint64_t owner = 0, sequence = 0;
    bool running = false;
    dfy::mc::BridgeConfig config;
    dfy_v2_query_interface_fn host = nullptr;
    dfy::mc::PartnerLoop partner;
    std::array<NativeJob, 4> jobs;
};

namespace {
// 每个实例只能在原宿主线程上操作，已关闭槽位不再持有 DLL 内对象。
thread_local std::array<dfy_v2_instance*, 16> slots{};
std::atomic_uint64_t serial{0};

struct PartnerSkill { std::string_view name, purpose, description, schema; bool changes; };
constexpr std::array<PartnerSkill, 4> partner_skills{{
    {"observe", "partner.observe", "Read your position, player position, landmarks, inventory and previous action. Optional block registry name scans visible nearby targets.",
        R"({"type":"object","properties":{"block":{"type":"string"}},"additionalProperties":false})", false},
    {"move", "partner.move", "Walk without digging to a named landmark, player (place=player), or one nearby integer x,y,z position.",
        R"({"type":"object","properties":{"place":{"type":"string"},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"additionalProperties":false})", true},
    {"collect", "partner.collect", "Gather visible blocks by Minecraft registry name and count (1-32). For a specific block supply x,y,z and count=1. Reports broken blocks and actual inventory changes.",
        R"({"type":"object","properties":{"block":{"type":"string"},"count":{"type":"integer"},"x":{"type":"integer"},"y":{"type":"integer"},"z":{"type":"integer"}},"required":["block","count"],"additionalProperties":false})", true},
    {"reply", "partner.reply", "Speak to the bound player in this game. Call only when you choose to speak. Internal turn completion is silent.",
        R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"],"additionalProperties":false})", true}
}};

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
        instance->host = host;
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
    raw->partner.reset();
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
    *value = owned(raw) && raw->running ? (raw->config.key.empty() ? 1u : 10u + static_cast<uint32_t>(partner_skills.size())) : 0u;
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
    } else if (index == 3 && !raw->config.key.empty()) {
        *out = {sizeof(*out), 1, DFY_V2_TOOL_CHANGES_STATE | DFY_V2_TOOL_IDEMPOTENT, 0, view("ask"), view("Ask the bound player about the observed material."),
            view(R"({"type":"object","properties":{"generation":{"type":"string"},"order":{"type":"integer"},"question":{"type":"string"}},"required":["generation","order","question"],"additionalProperties":false})")};
    } else if ((index == 4 || index == 5) && !raw->config.key.empty()) {
        const bool admit = index == 4;
        *out = {sizeof(*out), 1, admit ? DFY_V2_TOOL_CHANGES_STATE | DFY_V2_TOOL_IDEMPOTENT : 0u, 0,
            view(admit ? "admit" : "receipt"), view(admit ? "Admit the user's exact collection order." : "Read the original delivery receipt."),
            view(R"({"type":"object","properties":{"generation":{"type":"string"},"question":{"type":"string"},"grant":{"type":"string"},"owner":{"type":"string"},"order":{"type":"integer"},"quantity":{"type":"integer"},"material":{"type":"string"},"returnPlace":{"type":"string"},"via":{"type":"string"},"command":{"type":"string"}},"required":["generation","question","grant","owner","order","quantity","material","returnPlace","via","command"],"additionalProperties":false})")};
    } else if (index == 6 && !raw->config.key.empty()) {
        *out = {sizeof(*out), 1, DFY_V2_TOOL_CHANGES_STATE, 0, view("mine"), view("Mine one block under the admitted material order."),
            view(R"({"type":"object","properties":{"generation":{"type":"string"},"question":{"type":"string"},"grant":{"type":"string"},"index":{"type":"integer"}},"required":["generation","question","grant","index"],"additionalProperties":false})")};
    } else if (index == 7 && !raw->config.key.empty()) {
        *out = {sizeof(*out), 1, DFY_V2_TOOL_CHANGES_STATE, 0, view("return"), view("Return to the agreed meeting point and read the carried quantity."),
            view(R"({"type":"object","properties":{"generation":{"type":"string"},"question":{"type":"string"},"grant":{"type":"string"}},"required":["generation","question","grant"],"additionalProperties":false})")};
    } else if (index == 8 && !raw->config.key.empty()) {
        *out = {sizeof(*out), 1, 0, 0, view("survey"), view("Read the current world and observed exploration coverage."),
            view(R"({"type":"object","properties":{"order":{"type":"integer"},"inquiry":{"type":"string"}},"additionalProperties":false})")};
    } else if (index == 9 && !raw->config.key.empty()) {
        *out = {sizeof(*out), 1, DFY_V2_TOOL_CHANGES_STATE | DFY_V2_TOOL_IDEMPOTENT, 0, view("stop"), view("Release control of the original game actor."),
            view(R"({"type":"object","properties":{"generation":{"type":"string"}},"required":["generation"],"additionalProperties":false})")};
    } else if (index >= 10 && index - 10 < partner_skills.size() && !raw->config.key.empty()) {
        const auto& skill = partner_skills[index - 10];
        *out = {sizeof(*out), 1, skill.changes ? DFY_V2_TOOL_CHANGES_STATE : 0u, 0,
            view(skill.name), view(skill.description), view(skill.schema)};
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
    const bool ask = text(name) == "ask" && !raw->config.key.empty();
    const bool admit = text(name) == "admit" && !raw->config.key.empty();
    const bool receipt = text(name) == "receipt" && !raw->config.key.empty();
    const bool mine = text(name) == "mine" && !raw->config.key.empty();
    const bool home = text(name) == "return" && !raw->config.key.empty();
    const bool survey = text(name) == "survey" && !raw->config.key.empty();
    const bool control = text(name) == "stop" && !raw->config.key.empty();
    const auto skill = std::ranges::find(partner_skills,text(name),&PartnerSkill::name);
    const bool partner = !raw->config.key.empty() && skill != partner_skills.end();
    if (!inbox && !inspect && !ask && !admit && !receipt && !mine && !home && !survey && !control && !partner && text(name) != "world") return report(DFY_V2_NOT_FOUND, error);
    if (!call || call->struct_size != sizeof(*call) || call->tool_version != 1) return report(DFY_V2_ABI_MISMATCH, error);
    if (!call->session || !call->run || !call->step || !call->attempt || !call->invocation ||
        text(call->call_id).empty() || call->call_id.length > 128 || (text(name) == "world" && !empty_args(text(call->arguments))))
        return report(DFY_V2_INVALID_ARGUMENT, error);
    if ((partner || inspect || mine || home) && std::any_of(raw->jobs.begin(), raw->jobs.end(), [](const auto& job) { return job.sequence && job.action; }))
        return report(DFY_V2_BUSY, error);
    const auto found = std::find_if(raw->jobs.begin(), raw->jobs.end(), [](const auto& job) { return job.sequence == 0; });
    if (found == raw->jobs.end()) return report(DFY_V2_BUSY, error);
    auto& job = *found;
    if (raw->sequence == UINT64_MAX) return report(DFY_V2_RESOURCE_EXHAUSTED, error);
    try {
        job.id = std::to_string(raw->owner) + "." + std::to_string(raw->sequence + 1);
        job.arguments = text(call->arguments);
        if (partner) job.action = std::make_unique<dfy::mc::GameWork>(dfy::mc::GameAction::partner, raw->config,
            job.id, job.arguments, std::string(text(name)));
        else if (inspect || mine || home) job.action = std::make_unique<dfy::mc::GameWork>(
            mine ? dfy::mc::GameAction::mine : home ? dfy::mc::GameAction::home : dfy::mc::GameAction::inspect, raw->config, job.id, job.arguments);
        else if (control) job.wire = dfy::mc::control_request(job.id, raw->config.key, job.arguments);
        else if (survey) job.wire = dfy::mc::survey_request(job.id, raw->config.key, job.arguments);
        else if (ask) job.wire = dfy::mc::question_request(job.id, raw->config.key, job.arguments);
        else if (admit || receipt) job.wire = dfy::mc::delivery_request(admit, job.id, raw->config.key, job.arguments);
        else job.wire = inbox ? dfy::mc::inbox_request(job.id, raw->config.key, job.arguments) : dfy::mc::world_request(job.id);
    } catch (const std::bad_alloc&) { return report(DFY_V2_RESOURCE_EXHAUSTED, error); }
    catch (...) { return report(DFY_V2_INVALID_ARGUMENT, error); }
    if (control) job.method = NativeJob::Method::control;
    else if (survey) job.method = NativeJob::Method::survey;
    else if (admit) job.method = NativeJob::Method::admit;
    else if (receipt) job.method = NativeJob::Method::receipt;
    else if (ask) job.method = NativeJob::Method::ask;
    else if (inbox) job.method = NativeJob::Method::inbox;
    if (partner) {
        job.partner_session = call->session;
        job.invocation = call->invocation;
        job.skill = skill->name;
    }
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
    } else if (job->action) {
        try {
            if (auto value = job->action->poll(job->cancelled)) {
                job->output = std::move(value->payload);
                switch (value->effect) {
                case dfy::mc::GameEffect::observed: job->effect = DFY_V2_TOOL_APPLIED; break;
                case dfy::mc::GameEffect::none: job->effect = DFY_V2_TOOL_NOT_APPLIED; break;
                case dfy::mc::GameEffect::unknown: job->effect = DFY_V2_TOOL_UNKNOWN; break;
                }
                job->success = value->success && !job->cancelled;
                job->ready = true;
            }
        } catch (...) {
            job->effect = job->action->attempted() ? DFY_V2_TOOL_UNKNOWN : DFY_V2_TOOL_NOT_APPLIED;
            job->ready = true;
        }
    } else if (std::chrono::steady_clock::now() >= job->next) {
        try {
            if (!job->pipe) job->pipe = std::make_unique<dfy::mc::PipeExchange>(raw->config.channel, job->wire);
            job->attempted = true;
            if (auto reply = job->pipe->poll()) {
                switch (job->method) {
                case NativeJob::Method::control:
                    job->output = dfy::mc::control_reply(*reply, job->id, job->arguments);
                    job->effect = DFY_V2_TOOL_APPLIED;
                    if (job->output.empty()) {
                        job->pipe.reset();
                        job->next = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
                        *out = {sizeof(*out), 0, 0, job->effect, 0, 0, {}};
                        return report(DFY_V2_OK, error);
                    }
                    break;
                case NativeJob::Method::world: job->output = dfy::mc::world_reply(*reply, job->id); break;
                case NativeJob::Method::survey: job->output = dfy::mc::world_reply(*reply, job->id, true); break;
                case NativeJob::Method::inbox: job->output = dfy::mc::inbox_reply(*reply, job->id, job->arguments); break;
                case NativeJob::Method::ask:
                    job->output = dfy::mc::question_reply(*reply, job->id, job->arguments);
                    job->effect = DFY_V2_TOOL_APPLIED;
                    break;
                case NativeJob::Method::admit:
                case NativeJob::Method::receipt:
                    job->output = dfy::mc::delivery_reply(job->method == NativeJob::Method::admit, *reply, job->id, job->arguments);
                    if (job->method == NativeJob::Method::admit && job->output != "null") job->effect = DFY_V2_TOOL_APPLIED;
                    break;
                }
                job->success = job->ready = true;
            }
        } catch (...) {
            // 旧桥接只提供普通世界快照，缺少探索依据时仍保留位置查询。
            if (job->method == NativeJob::Method::survey) {
                try {
                    job->pipe.reset(); job->wire = dfy::mc::world_request(job->id); job->method = NativeJob::Method::world;
                    *out = {sizeof(*out), 0, 0, DFY_V2_TOOL_NOT_APPLIED, 0, 0, {}};
                    return report(DFY_V2_OK, error);
                } catch (...) {}
            }
            if ((job->method == NativeJob::Method::ask || job->method == NativeJob::Method::admit || job->method == NativeJob::Method::control) && job->attempted)
                job->effect = DFY_V2_TOOL_UNKNOWN;
            job->ready = true;
        }
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
        if ((job->method == NativeJob::Method::ask || job->method == NativeJob::Method::admit || job->method == NativeJob::Method::control) && job->attempted)
            job->effect = DFY_V2_TOOL_UNKNOWN;
        if (!job->action) job->ready = true;
    }
    return report(DFY_V2_OK, error);
}
// 归还已退出的票据，未退出工作保持占用。
dfy_v2_status DFY_V2_CALL release(dfy_v2_instance* raw, dfy_v2_tool_ticket ticket, dfy_v2_error* error) noexcept {
    auto* job = find_job(raw, ticket);
    if (!job) return report(DFY_V2_INVALID_HANDLE, error);
    if (!job->exited) return report(DFY_V2_BUSY, error);
    if (job->partner_session) raw->partner.record(job->partner_session,job->invocation,job->skill,job->effect,std::move(job->output));
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
// 将业务用途映射到本实例实际登记的工具，未配对时不暴露私有或移动入口。
dfy_v2_status DFY_V2_CALL route(dfy_v2_instance* raw, dfy_v2_string_view purpose, dfy_v2_string_view* out, dfy_v2_error* error) noexcept {
    if (!out) return report(DFY_V2_INVALID_ARGUMENT, error);
    *out = {};
    if (!owned(raw) || !raw->running) return report(DFY_V2_INVALID_CONTEXT, error);
    if (!raw->config.key.empty()) {
        for (const auto& skill : partner_skills) if (text(purpose) == skill.purpose) {
            *out = view(skill.name);
            return report(DFY_V2_OK, error);
        }
        if (text(purpose) == "player.inbox") *out = view("inbox");
        else if (text(purpose) == "control.release") *out = view("stop");
        else if (text(purpose) == "search.progress") *out = view("survey");
        else if (text(purpose) == "visit.inspect") *out = view("inspect");
        else if (text(purpose) == "question.open") *out = view("ask");
        else if (text(purpose) == "collect.admit") *out = view("admit");
        else if (text(purpose) == "collect.receipt") *out = view("receipt");
        else if (text(purpose) == "collect.mine") *out = view("mine");
        else if (text(purpose) == "collect.return") *out = view("return");
    }
    return report(out->data ? DFY_V2_OK : DFY_V2_NOT_FOUND, error);
}
const dfy_v2_companion_routes_api routes{{sizeof(dfy_v2_companion_routes_api), 1}, route};
// 只枚举本实例显式提供给伙伴模型的技能，保持宿主控制工具私有。
dfy_v2_status DFY_V2_CALL skill_at(dfy_v2_instance* raw, uint32_t index, dfy_v2_string_view* out, dfy_v2_error* error) noexcept {
    if (!out) return report(DFY_V2_INVALID_ARGUMENT, error);
    *out = {};
    if (!owned(raw) || !raw->running) return report(DFY_V2_INVALID_CONTEXT, error);
    if (raw->config.key.empty() || index >= partner_skills.size()) return report(DFY_V2_NOT_FOUND, error);
    *out = view(partner_skills[index].name);
    return report(DFY_V2_OK, error);
}
const dfy_v2_companion_skills_api skills{{sizeof(dfy_v2_companion_skills_api), 1}, skill_at};

// 把显式使用伙伴的请求交给本实例外层循环。
dfy_v2_status DFY_V2_CALL listen_partner(dfy_v2_instance* raw, dfy_v2_error* error) noexcept {
    if (!owned(raw) || !raw->running) return report(DFY_V2_INVALID_CONTEXT,error);
    try { return report(raw->partner.listen(raw->host,raw->config),error); }
    catch (...) { return report(DFY_V2_FAILED,error); }
}

// 只请求已有伙伴会话退出，后台步骤负责继续收尾。
dfy_v2_status DFY_V2_CALL stop_partner(dfy_v2_instance* raw, dfy_v2_error* error) noexcept {
    if (!owned(raw) || !raw->running) return report(DFY_V2_INVALID_CONTEXT,error);
    raw->partner.stop();
    return report(DFY_V2_OK,error);
}

// 读取插件当前伙伴状态，字符串视图不脱离原调用。
dfy_v2_status DFY_V2_CALL partner_status(dfy_v2_instance* raw, dfy_v2_partner_status* out, dfy_v2_error* error) noexcept {
    if (!out) return report(DFY_V2_INVALID_ARGUMENT,error);
    *out = {};
    if (!owned(raw) || !raw->running) return report(DFY_V2_INVALID_CONTEXT,error);
    *out = raw->partner.status();
    return report(DFY_V2_OK,error);
}

// 在公平调度下推进一小步，空闲时降低宿主轮询频率。
dfy_v2_status DFY_V2_CALL advance_partner(dfy_v2_instance* raw, uint32_t* wait, dfy_v2_error* error) noexcept {
    if (!wait) return report(DFY_V2_INVALID_ARGUMENT,error);
    *wait = 0;
    if (!owned(raw) || !raw->running) return report(DFY_V2_INVALID_CONTEXT,error);
    try {
        dfy::mc::PartnerWork work;
        const auto session = raw->partner.status().session;
        for (const auto& job : raw->jobs) if (session && job.partner_session == session && job.sequence && !job.ready &&
            (job.skill == "move" || job.skill == "collect")) work = {job.invocation,job.skill};
        raw->partner.advance(raw->config,raw->owner,work);
        *wait = raw->partner.status().phase == DFY_V2_PARTNER_CLOSED ? 100 : 5;
        return report(DFY_V2_OK,error);
    } catch (...) { return report(DFY_V2_FAILED,error); }
}

const dfy_v2_companion_partner_api partner{{sizeof(dfy_v2_companion_partner_api),DFY_V2_COMPANION_PARTNER_VERSION},
    listen_partner,stop_partner,partner_status};
const dfy_v2_activity_api activity{{sizeof(dfy_v2_activity_api),DFY_V2_ACTIVITY_VERSION},advance_partner};

// 伙伴只控制自己的旧模型与待开始技能，已发生效果由工具照常记录。
dfy_v2_status DFY_V2_CALL describe_partner_hook(dfy_v2_instance* raw, uint32_t index, dfy_v2_hook_description* out,
    dfy_v2_error* error) noexcept {
    if (!out) return report(DFY_V2_INVALID_ARGUMENT,error);
    *out = {};
    if (!owned(raw) || !raw->running) return report(DFY_V2_INVALID_CONTEXT,error);
    if (index > 1 || raw->config.key.empty()) return report(DFY_V2_NOT_FOUND,error);
    *out = {index ? DFY_V2_HOOK_TOOL_START : DFY_V2_HOOK_MODEL_REQUEST,DFY_V2_HOOK_OWN_SESSION};
    return report(DFY_V2_OK,error);
}

// 使用当前事件和接管状态裁定尚未接纳的旧步骤。
dfy_v2_status DFY_V2_CALL decide_partner_hook(dfy_v2_instance* raw, uint32_t index, const dfy_v2_hook_input* input,
    uint32_t* verdict, dfy_v2_error* error) noexcept {
    if (!verdict) return report(DFY_V2_INVALID_ARGUMENT,error);
    *verdict = 0;
    if (!owned(raw) || !raw->running || !input || index > 1) return report(DFY_V2_INVALID_CONTEXT,error);
    *verdict = raw->partner.permits(input->session) ? DFY_V2_HOOK_ALLOW : DFY_V2_HOOK_DENY;
    return report(DFY_V2_OK,error);
}
const dfy_v2_hook_api hooks{{sizeof(dfy_v2_hook_api),DFY_V2_HOOK_VERSION},describe_partner_hook,decide_partner_hook};
// 返回固定版本的静态能力表。
dfy_v2_status DFY_V2_CALL query(dfy_v2_string_view name, uint32_t version, const void** out, dfy_v2_error* error) noexcept {
    if (!out) return report(DFY_V2_INVALID_ARGUMENT, error);
    *out = nullptr;
    if (version == 1 && text(name) == DFY_V2_LIFECYCLE_INTERFACE) *out = &lifecycle;
    if (version == 1 && text(name) == DFY_V2_TOOL_INTERFACE) *out = &tools;
    if (version == DFY_V2_COMPANION_VERSION && text(name) == DFY_V2_COMPANION_INTERFACE) *out = &companion;
    if (version == DFY_V2_COMPANION_ROUTES_VERSION && text(name) == DFY_V2_COMPANION_ROUTES_INTERFACE) *out = &routes;
    if (version == DFY_V2_COMPANION_SKILLS_VERSION && text(name) == DFY_V2_COMPANION_SKILLS_INTERFACE) *out = &skills;
    if (version == DFY_V2_COMPANION_PARTNER_VERSION && text(name) == DFY_V2_COMPANION_PARTNER_INTERFACE) *out = &partner;
    if (version == DFY_V2_ACTIVITY_VERSION && text(name) == DFY_V2_ACTIVITY_INTERFACE) *out = &activity;
    if (version == DFY_V2_HOOK_VERSION && text(name) == DFY_V2_HOOK_INTERFACE) *out = &hooks;
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
