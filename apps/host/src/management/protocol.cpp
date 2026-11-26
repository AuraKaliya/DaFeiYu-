#include "protocol.hpp"
#include "host.hpp"
#include "message_protocol.hpp"
#include "mailbox_protocol.hpp"
#include "plugin_directory.hpp"
#include <nlohmann/json.hpp>
#include <charconv>

namespace dfy::management {
namespace {
// 读取完整非零十进制身份，保留 64 位精度。
std::uint64_t run_id_part(const nlohmann::json& value) {
    const auto& text = value.get_ref<const std::string&>();
    std::uint64_t id = 0;
    const auto result = std::from_chars(text.data(),text.data()+text.size(),id);
    if (result.ec != std::errc{} || result.ptr != text.data()+text.size() || !id)
        throw std::invalid_argument("UI: invalid run identity");
    return id;
}

// 将原业务及实际退出状态转换为稳定的界面状态名。
std::string_view operation_state(runtime::OperationState state) noexcept {
    switch (state) {
    case runtime::OperationState::pending: return "pending";
    case runtime::OperationState::succeeded: return "succeeded";
    case runtime::OperationState::failed: return "failed";
    case runtime::OperationState::cancelled: return "cancelled";
    }
    return "unknown";
}

// 将窗口命令状态映射成固定标签，结果及实际退出来自独立的回合条目。
std::string_view dialogue_state(DialogueCommandState state) noexcept {
    using State = DialogueCommandState;
    switch (state) {
    case State::unavailable: return "unavailable";
    case State::queued: return "queued";
    case State::accepted: return "accepted";
    case State::busy: return "busy";
    case State::rejected: return "rejected";
    case State::conflict: return "conflict";
    case State::expired: return "expired";
    case State::stale: return "stale";
    case State::full: return "full";
    }
    return "rejected";
}

// 以十进制字符串返回原命令、正式回合和待发消息身份。
nlohmann::json dialogue_receipt(const DialogueReceipt& receipt) {
    nlohmann::json result{{"command",std::to_string(receipt.command)},{"state",dialogue_state(receipt.state)},
        {"session",std::to_string(receipt.run.session)},{"turn",std::to_string(receipt.run.sequence)},{"message",std::to_string(receipt.message)}};
    if (receipt.stored) result["storedMessage"] = std::to_string(receipt.stored);
    return result;
}

// 按固定命令名逐项核对字段，只从对应入口读取正文、原回合或待发消息。
// 步骤：解析命令身份 → 验证该命令的字段数量与目标 → 交付带类型的业务命令。
DialogueCommand dialogue_command(const nlohmann::json& request, std::string_view type) {
    DialogueCommand command{run_id_part(request.at("command")),DialogueAction::send,{},{}};
    if (type == "dialogue.submit" && request.size() == 5) {
        command.text = request.at("text").get<std::string>();
        return command;
    }
    command.target.session = run_id_part(request.at("session"));
    if ((type == "dialogue.resume" || type == "dialogue.remove") && request.size() == 6) {
        command.action = type == "dialogue.resume" ? DialogueAction::resume : DialogueAction::remove;
        command.message = run_id_part(request.at("message"));
        return command;
    }
    command.target.sequence = run_id_part(request.at("turn"));
    if ((type == "dialogue.enqueue" || type == "dialogue.append") && request.size() == 7) {
        command.action = type == "dialogue.enqueue" ? DialogueAction::enqueue : DialogueAction::append;
        command.text = request.at("text").get<std::string>();
    } else if ((type == "dialogue.cancel" || type == "dialogue.clear") && request.size() == 6) {
        command.action = type == "dialogue.cancel" ? DialogueAction::cancel : DialogueAction::clear;
    } else throw std::invalid_argument("UI: invalid dialogue command");
    return command;
}

// 区分普通回合、等待读回和已裁定的读回证据。
nlohmann::json verification_state(const std::optional<app::VerificationReport>& report) {
    if (!report) return nullptr;
    if (!report->match) return "pending";
    switch (*report->match) {
    case agent::ObservationMatch::satisfied: return "satisfied";
    case agent::ObservationMatch::not_satisfied: return "not_satisfied";
    case agent::ObservationMatch::unknown: return "unknown";
    }
    return "unknown";
}

// 从原生房间取得绑定位置及实际身体采样。
nlohmann::json room_snapshot(const Host& host) {
    nlohmann::json room = nullptr;
    if (const auto location = host.room_location()) {
        const auto& self = location->self;
        room = {{"instance",std::to_string(self.room.instance)},{"generation",std::to_string(self.room.generation)},
            {"layoutVersion",std::to_string(location->layout_version)},{"body",std::to_string(self.body)},
            {"revision",std::to_string(self.revision)},{"roomName",location->room_name},{"placeName",location->place_name},
            {"x",self.position.x},{"y",self.position.y},{"activity",self.activity == app::RoomActivity::walking ? "walking" : "resting"}};
    }
    return room;
}

// 通过同一命令入口交付聊天意图，按已读修订返回对话与队列。
// 步骤：区分读写命令 → 校验命令或按修订装配正文 → 返回对应的命令回执。
nlohmann::json dialogue_response(Host& host, const nlohmann::json& request, const std::string& type, const std::string& id,
    const std::string& client) {
    nlohmann::json reply{{"version","1"},{"request",id}};
    if (type != "dialogue.read") {
        reply["event"] = "dialogue.receipt";
        auto base = request;
        const bool stable = request.contains("submission");
        if (stable) {
            base.erase("submission");
            base.erase("instance");
            base.erase("sourceUtcMs");
        }
        auto command = dialogue_command(base,type);
        if (stable) {
            if (type!="dialogue.submit" && type!="dialogue.append" && type!="dialogue.enqueue")
                throw std::invalid_argument("messages: expected a text operation");
            command.submission = request.at("submission").get<std::string>();
            if (command.submission.empty() || command.submission.size()>128) throw std::invalid_argument("messages: invalid submission");
            if (type!="dialogue.submit") command.instance = request.at("instance").get<std::string>();
            if ((type=="dialogue.submit" && request.contains("instance")) || (type!="dialogue.submit" && command.instance.empty()) ||
                command.instance.size()>64) throw std::invalid_argument("messages: invalid instance");
            if (request.contains("sourceUtcMs")) {
                const auto& utc = request.at("sourceUtcMs");
                if (!utc.is_number_integer() || utc<0 || utc>INT64_MAX) throw std::invalid_argument("messages: invalid source timestamp");
                command.source_utc_ms = utc.get<std::int64_t>();
            }
        }
        command.client = client;
        reply["receipt"] = dialogue_receipt(host.submit_dialogue(std::move(command)));
    } else {
        if (request.size() != 5) throw std::invalid_argument("management: invalid dialogue read");
        const auto& after = request.at("revision").get_ref<const std::string&>();
        if (after.size() > 20) throw std::invalid_argument("management: invalid dialogue read");
        const auto& command = request.at("command");
        const auto view = host.dialogue(command == "" ? 0 : run_id_part(command),client);
        const auto revision = view.display ? std::to_string(view.display->revision) : std::string();
        nlohmann::json turns = nullptr, queue = nullptr;
        if (view.display && revision != after) {
            turns = nlohmann::json::array();
            for (const auto& turn : view.display->turns) {
                auto inputs = nlohmann::json::array();
                for (const auto& input : turn.inputs)
                    inputs.push_back({{"message",std::to_string(input.message)},{"text",input.text},{"state",agent::input_state_name(input.state)},
                        {"modelStep",std::to_string(input.model_step)}});
                turns.push_back({{"session",std::to_string(turn.run.session)},{"turn",std::to_string(turn.run.sequence)},
                    {"prompt",turn.prompt},{"answer",turn.answer},{"result",operation_state(turn.result)},{"exit",operation_state(turn.exit)},
                    {"queuedMessage",std::to_string(turn.queued_message)},{"acceptsInput",turn.accepts_input},{"inputs",std::move(inputs)}});
            }
            queue = nlohmann::json::array();
            for (const auto& item : view.display->queue)
                queue.push_back({{"message",std::to_string(item.message)},{"session",std::to_string(item.session)},{"text",item.text}});
        }
        reply.update({{"event","dialogue.snapshot"},{"ready",bool(view.display)},{"accepting",view.accepting},
            {"latestCommand",std::to_string(view.latest_command)},{"revision",revision},{"turns",std::move(turns)},
            {"queue",std::move(queue)},{"queuePaused",view.display && view.display->queue_paused},
            {"receipt",view.receipt ? dialogue_receipt(*view.receipt) : nlohmann::json(nullptr)}});
        if (const auto high = host.message_watermark(client)) reply["messageWatermark"] = std::to_string(*high);
    }
    return reply;
}

// 按可选回合过滤公开运行摘要，不携带聊天正文。
nlohmann::json run_snapshot(const Host* host, const nlohmann::json& request) {
    std::optional<agent::RunId> expected;
    if (request.size() == 6) expected = agent::RunId{run_id_part(request.at("session")),run_id_part(request.at("turn"))};

    const auto snapshot = host ? host->snapshot() : std::nullopt;
    nlohmann::json run = nullptr;
    if (snapshot && (!expected || *expected == agent::RunId{snapshot->status.session,snapshot->status.turn})) {
        const auto& status = snapshot->status;
        run = {{"session",std::to_string(status.session)},{"turn",std::to_string(status.turn)},
            {"result",operation_state(status.result)},{"exit",operation_state(status.exit)},
            {"waiting_confirmations",status.waiting_confirmations},{"verification",verification_state(snapshot->verification)}};
    }
    return run;
}
}

// 拒绝前导零、尾随字符和越过整数范围的协议修订或身份。
std::uint64_t management_number(const nlohmann::json& value) {
    const auto& text = value.get_ref<const std::string&>();
    std::uint64_t number = 0;
    const auto parsed = std::from_chars(text.data(),text.data()+text.size(),number);
    if (text.size()>20 || parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || (text.size()>1 && text.front()=='0'))
        throw std::invalid_argument("management: invalid numeric identity");
    return number;
}

// 在正式本体入口解析一次版本化请求，读取状态或提交具体对话命令。
// 步骤：校验版本与身份 → 区分公开查询和会话请求 → 经宿主处理并回传原请求号。
nlohmann::json dispatch(Host* host, const nlohmann::json& request, const std::string& client) {
    if (!request.is_object() || request.at("version") != "1") throw std::invalid_argument("management: invalid protocol");
    const auto& id = request.at("request").get_ref<const std::string&>();
    const auto& type = request.at("type").get_ref<const std::string&>();
    if (id.empty() || id.size()>64) throw std::invalid_argument("management: invalid request identity");

    if (type == "state.read" && (request.size()==4 || request.size()==6) && request.at("topic")=="run")
        return {{"event","state.snapshot"},{"version","1"},{"topic","run"},{"request",id},
            {"available",host != nullptr},{"run",run_snapshot(host,request)}};
    if (!host) throw std::runtime_error("management: host unavailable");
    if (type.starts_with("settings.")) return settings_request(*host,request,client);
    if (type=="plugins.list") return plugin_directory_request(*host,request);
    if (type.starts_with("plugins.")) return plugin_action_request(*host,request,client);
    if (type.starts_with("mailbox.")) return mailbox_request(*host,request);
    if (type.starts_with("messages.")) return message_request(*host,request,client);
    if (type == "room.read" && request.size()==3)
        return {{"event","room.snapshot"},{"version","1"},{"request",id},{"room",room_snapshot(*host)}};
    if (type.starts_with("dialogue.")) return dialogue_response(*host,request,type,id,client);
    throw std::invalid_argument("management: unknown request");
}
}
