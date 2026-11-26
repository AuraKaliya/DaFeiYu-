#include "host.hpp"
#include "software_log.hpp"
#include "message_store.hpp"
#include "main_messages.hpp"
#include "mailbox_view.hpp"
#include "plugin_directory.hpp"
#include <dafeiyu/digest.hpp>
#include <dafeiyu/utf8.hpp>
#include <algorithm>
#include <cstdio>
#include <random>
#include <utility>

namespace dfy::management {
namespace {
constexpr auto interval = std::chrono::milliseconds(5);
constexpr std::size_t command_limit = 16;

// 以稳定的原实例和用户原话建立跨连接可比较的载荷。
nlohmann::json envelope(const DialogueCommand& command) {
    const auto route = command.action==DialogueAction::send ? "send" : command.action==DialogueAction::append ? "append" : "enqueue";
    return app::user_envelope(route,command.text,command.instance,command.target,command.source_utc_ms);
}

// 入库前按后台已发布的目标和容量核验新文字，实际交付仍复核原运行。
// 步骤：核对原实例与回合 → 按去向核对当前状态 → 检查条数及剩余字节。
std::optional<DialogueCommandState> input_refusal(const DialogueCommand& command, const std::string& instance,
    const app::ConversationDisplay& display) {
    using State = DialogueCommandState;
    if (command.action==DialogueAction::send) return {};
    if (command.instance!=instance || display.turns.empty() || display.turns.back().run!=command.target) return State::stale;
    if (command.action==DialogueAction::enqueue) {
        if (!app::dialogue_queue_fits(display.queue,command.text.size())) return State::full;
        return {};
    }

    const auto& turn = display.turns.back();
    if (turn.result!=runtime::OperationState::pending || turn.exit!=runtime::OperationState::pending) return State::stale;
    if (!turn.accepts_input) return State::unavailable;
    std::size_t bytes = command.text.size();
    for (const auto& input : turn.inputs) bytes += input.text.size();
    if (turn.inputs.size()>=agent::turn_input_count_limit || bytes>agent::turn_input_byte_limit) return State::full;
    return {};
}

// 将持久投递回执映射回旧窗口的命令语义，待发号与持久消息号各自保留。
DialogueReceipt stored_receipt(DialogueReceipt original, DialogueAction action, const nlohmann::json& result) {
    using State = DialogueCommandState;
    const auto& label = result.at("state");
    if (label=="accepted" || label=="queued" || label=="waiting" || label=="submitted") original.state = State::accepted;
    else if (label=="pending" || label=="dispatching") original.state = State::queued;
    else if (label=="full" || label=="limit") original.state = State::full;
    else if (label=="stale" || label=="closed" || label=="removed") original.state = State::stale;
    else if (label=="unknown" || label=="unavailable") original.state = State::unavailable;
    else original.state = State::rejected;
    if (result.contains("session"))
        original.run = {std::stoull(result.at("session").get<std::string>()),std::stoull(result.at("turn").get<std::string>())};
    if (action==DialogueAction::enqueue) original.run.sequence = 0;
    if (result.contains("nativeMessage")) original.message = std::stoull(result.at("nativeMessage").get<std::string>());
    return original;
}

// 依受信页面提供的回合或消息身份，在业务线程执行一次对话操作。
// 步骤：发送或排队交给应用 → 按原待发消息恢复或移除 → 对原回合提交停止或清空。
DialogueReceipt apply_dialogue(app::Application& application, const DialogueCommand& command) {
    using State = DialogueCommandState;
    if (command.stored)
        return stored_receipt({command.id,State::queued,command.target,0,command.stored},command.action,
            application.main_messages()->deliver(command.stored,envelope(command),command.stored_utc_ms));
    if (command.action == DialogueAction::send) {
        const auto result = application.submit_dialogue(command.text);
        return {command.id,State::accepted,{result.session,result.turn}};
    }

    auto& conversation = application.conversation();
    if (command.action == DialogueAction::append) {
        const auto receipt = application.append_dialogue(command.target,command.text);
        State state = State::rejected;
        switch (receipt.state) {
        case agent::InputAdmission::accepted: state = State::accepted; break;
        case agent::InputAdmission::closed: state = State::stale; break;
        case agent::InputAdmission::full: state = State::full; break;
        case agent::InputAdmission::unavailable: state = State::unavailable; break;
        case agent::InputAdmission::rejected: break;
        }
        return {command.id,state,command.target,receipt.message};
    }
    if (command.action == DialogueAction::enqueue) {
        const auto receipt = application.enqueue_dialogue(command.target,command.text);
        State state = State::rejected;
        switch (receipt.state) {
        case app::QueueAdmission::accepted: state = State::accepted; break;
        case app::QueueAdmission::full: state = State::full; break;
        case app::QueueAdmission::stale: state = State::stale; break;
        case app::QueueAdmission::rejected: break;
        }
        return {command.id,state,{command.target.session,0},receipt.message};
    }
    if (command.action == DialogueAction::remove || command.action == DialogueAction::resume) {
        const bool matched = command.target.session == conversation.id() && (command.action == DialogueAction::remove
            ? application.remove_queued_dialogue(command.message) : application.resume_dialogue_queue(command.message));
        return {command.id,matched ? State::accepted : State::stale,command.target,command.message};
    }
    const auto& turns = conversation.display().turns;
    if (turns.empty() || turns.back().run != command.target) return {command.id,State::stale,command.target};
    if (command.action == DialogueAction::cancel) {
        const auto current = application.run_status(command.target);
        if (!current || !conversation.cancel()) return {command.id,State::stale,command.target};
    } else {
        if (conversation.busy()) return {command.id,State::busy,command.target};
        conversation.clear();
    }
    return {command.id,State::accepted,command.target};
}

}

// 先等待会话退出，再推进原插件停止及回收边界。
// 步骤：关闭会话接纳 → 等待实际退出 → 有界推进插件关闭并检查结果。
void Host::shutdown(app::Application& application) {
    auto& conversation = application.conversation();
    const auto current = conversation.status();
    const auto run = current && current->exit==runtime::OperationState::pending ? agent::RunId{current->session,current->turn} : agent::RunId{};
    set_stop_phase(StopPhase::conversation,run);
    conversation.close();
    while (!conversation.closed()) {
        application.advance_plugins();
        conversation.advance();
        std::this_thread::sleep_for(interval);
    }

    set_stop_phase(StopPhase::plugins);
    const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(5);
    auto result = application.close_plugins();
    while (result != plugins::ShutdownResult::closed) {
        if (result == plugins::ShutdownResult::failed || std::chrono::steady_clock::now() >= deadline ||
            application.advance_plugins() == runtime::RunResult::failed)
            throw std::runtime_error("UI: plugin shutdown incomplete");
        std::this_thread::sleep_for(interval);
        result = application.close_plugins();
    }
    if (application.main_messages()) application.main_messages()->finish();
    set_stop_phase(StopPhase::complete);
}

// 将创建和推进入口移交新业务线程，线程的寿命短于快照存储。
Host::Host(Open open, Drive drive, app::SoftwareLog* software, std::shared_ptr<app::MessageStore> messages, std::string instance)
    : messages_(std::move(messages)), instance_(std::move(instance)), software_(software),
      worker_([this,open = std::move(open),drive = std::move(drive)](std::stop_token stop) { run(stop,open,drive); }) {}

// 在同一窗口锁内登记订阅和采样水位，之后的提交可按客户端自己的游标续读。
std::optional<std::int64_t> Host::message_watermark(const std::string& client, bool subscribe) const {
    std::lock_guard lock(mutex_);
    const auto owner = clients_.find(client);
    if (!messages_ || owner==clients_.end()) return {};
    if (subscribe) owner->second.subscribed = true;
    return owner->second.subscribed ? std::optional(messages_->watermark()) : std::nullopt;
}

// 在通知业务线程前固定本次停止是否源自显式退出。
void Host::close(bool explicit_request) noexcept {
    if (explicit_request) explicit_stop_ = true;
    worker_.request_stop();
    std::lock_guard lock(mutex_);
    for (auto& op : plugin_operations_) if (op->state=="queued") { op->state="unavailable"; op->effect="notApplied"; }
    queued_plugins_ = 0;
}

// 复制最近发布的阶段、开始点和等待对象。
StopProgress Host::stop_progress() const {
    std::lock_guard lock(mutex_);
    return progress_;
}

// 按实际业务边界发布等待对象，并记录上一个阶段结束时的原回合和耗时。
// 步骤：原子替换阶段快照 → 结算原阶段 → 登记新的等待对象。
void Host::set_stop_phase(StopPhase phase, agent::RunId run) {
    const auto now = std::chrono::steady_clock::now();
    StopProgress previous;
    {
        std::lock_guard lock(mutex_);
        previous = std::exchange(progress_,StopProgress{phase,run,now});
    }
    if (!software_) return;
    const auto event = [](StopPhase item) { return item==StopPhase::conversation ? app::SoftwareEvent::conversation_drain : app::SoftwareEvent::plugins_drain; };
    if (previous.phase==StopPhase::conversation || previous.phase==StopPhase::plugins) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now-previous.since).count();
        software_->record(event(previous.phase),phase==StopPhase::failed ? app::SoftwareResult::failed : app::SoftwareResult::ok,
            {.shutdown=explicit_stop_.load(),.duration_ms=elapsed,.session=previous.run.session,.turn=previous.run.sequence});
    }
    if (phase==StopPhase::conversation || phase==StopPhase::plugins)
        software_->record(event(phase),app::SoftwareResult::started,{.shutdown=explicit_stop_.load(),.session=run.session,.turn=run.sequence});
}

// 返回脱离业务对象寿命的当前值。
std::optional<app::RunStatusSnapshot> Host::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

// 读取最近的本体位置，查询不驱动原生房间。
std::optional<RoomLocation> Host::room_location() const {
    std::lock_guard lock(mutex_);
    return room_location_;
}

// 给连接生成跨进程重启可区分的身份，回收长时间未访问的观察者名额。
// 步骤：生成随机身份 → 核对客户端容量与空闲时间 → 登记独立水位。
std::string Host::open_client() {
    std::random_device random;
    std::string id = "client";
    for (unsigned part=0; part<4; ++part) id += "-"+std::to_string(random());
    std::lock_guard lock(mutex_);
    if (clients_.size()>=33) {
        const auto stale = std::chrono::steady_clock::now()-std::chrono::minutes(5);
        std::erase_if(clients_,[&](const auto& entry) {
            if (entry.first.empty() || entry.second.seen>=stale) return false;
            terminal_.disconnect(entry.first);
            return true;
        });
    }
    if (clients_.size()>=33 || !clients_.try_emplace(id).second) throw std::runtime_error("management: client unavailable");
    return id;
}

// 关闭远端观察者身份，保留未完成命令的宿主责任。
void Host::close_client(const std::string& client) {
    std::lock_guard lock(mutex_);
    terminal_.disconnect(client);
    if (!client.empty()) clients_.erase(client);
}

// 只续期仍有效的远端观察身份，空值保留给进程内适配器。
bool Host::visit_client(const std::string& client) {
    std::lock_guard lock(mutex_);
    const auto found = clients_.find(client);
    if (client.empty() || found == clients_.end()) return false;
    found->second.seen = std::chrono::steady_clock::now();
    return true;
}

// 窗口命令只固定与持久消息的关联，每次查询读取已经提交的实际状态。
DialogueReceipt Host::current_receipt(const CommandRecord& record) const {
    if (!record.receipt.stored) return record.receipt;
    return stored_receipt(record.receipt,record.action,messages_->receipt(record.receipt.stored));
}

// 返回共享快照，原回执受客户端身份与独立水位约束。
// 步骤：核对客户端 → 复制已发布快照 → 查找该客户端原命令或过期状态。
DialogueView Host::dialogue(std::uint64_t command, const std::string& client) const {
    std::lock_guard lock(mutex_);
    const auto owner = clients_.find(client);
    if (owner == clients_.end()) return {};
    owner->second.seen = std::chrono::steady_clock::now();
    DialogueView view{display_,owner->second.latest,can_submit_ && !exited_ && !worker_.get_stop_token().stop_requested(),{}};
    if (command && command <= owner->second.latest) {
        const auto& commands = owner->second.commands;
        const auto record = std::ranges::find_if(commands,[&](const auto& item) { return item.receipt.command==command; });
        view.receipt = record != commands.end() ? current_receipt(*record) : DialogueReceipt{command,DialogueCommandState::expired,{}};
    }
    return view;
}

// 签收页面命令并保留原目标，只有业务线程可以改变会话。
// 步骤：校验参数 → 复用原身份或报告冲突 → 核对槽位及忙碌状态 → 登记待交付命令。
DialogueReceipt Host::submit_dialogue(DialogueCommand command) {
    using State = DialogueCommandState;
    const bool with_text = command.action == DialogueAction::send || command.action == DialogueAction::enqueue || command.action == DialogueAction::append;
    const bool by_message = command.action == DialogueAction::resume || command.action == DialogueAction::remove;
    const bool target_valid = command.action == DialogueAction::send ? command.target == agent::RunId{} :
        command.target.session && (by_message ? !command.target.sequence : command.target.sequence != 0);
    if (!command.id || !target_valid || by_message != (command.message != 0) ||
        (with_text ? command.text.size() > app::dialogue_text_limit || command.text.find_first_not_of(" \t\r\n") == std::string::npos ||
            !runtime::valid_utf8(command.text) : !command.text.empty()))
        return {command.id,State::rejected,{}};
    auto fingerprint = with_text ? platform::sha256(command.text) : std::string{};
    if (!command.submission.empty()) {
        nlohmann::json identity{fingerprint,command.submission,command.instance};
        if (command.source_utc_ms) identity.push_back(*command.source_utc_ms);
        fingerprint = platform::sha256(identity.dump());
    }

    std::lock_guard lock(mutex_);
    const auto owner = clients_.find(command.client);
    if (owner == clients_.end()) return {command.id,State::unavailable,{}};
    owner->second.seen = std::chrono::steady_clock::now();
    auto& commands = owner->second.commands;
    if (command.id <= owner->second.latest) {
        const auto record = std::ranges::find_if(commands,[&](const auto& item) { return item.receipt.command==command.id; });
        if (record == commands.end()) return {command.id,State::expired,{}};
        if (record->action != command.action || record->target != command.target || record->message != command.message ||
            record->fingerprint != fingerprint)
            return {command.id,State::conflict,{}};
        return current_receipt(*record);
    }
    if (!display_ || exited_ || worker_.get_stop_token().stop_requested()) return {command.id,State::unavailable,{}};

    auto& slot = command.action == DialogueAction::cancel ? stop_command_ : pending_;
    const bool needs_idle = command.action == DialogueAction::send || command.action == DialogueAction::clear;
    bool available = !slot && (!needs_idle || can_submit_);
    DialogueReceipt receipt{command.id,available ? State::queued : State::busy,command.target};
    if (messages_ && with_text) {
        if (command.submission.empty()) command.submission = (command.client.empty() ? instance_ : command.client)+"/"+std::to_string(command.id);
        if (command.instance.empty()) command.instance = instance_;
        const auto original = envelope(command);
        try {
            if (const auto prior = messages_->find("desktop",command.submission)) {
                if (prior->envelope!=original) return {command.id,State::conflict,{}};
                receipt.stored = prior->id;
                receipt = stored_receipt(receipt,command.action,prior->receipt);
                available = false;
            } else if (available) {
                if (const auto refused = input_refusal(command,instance_,*display_)) {
                    receipt.state = *refused;
                    available = false;
                } else {
                    const auto accepted = messages_->put("desktop",command.submission,original,{{"state","pending"}});
                    command.stored = receipt.stored = accepted.message.id;
                    command.stored_utc_ms = accepted.message.utc_ms;
                }
            }
        } catch (const std::runtime_error&) {
            if (software_) software_->record(app::SoftwareEvent::message_storage,app::SoftwareResult::failed);
            return {command.id,State::unavailable,command.target};
        }
    }
    commands.push_back({receipt,command.action,command.target,command.message,std::move(fingerprint)});
    owner->second.latest = command.id;
    if (available) {
        slot.emplace(std::move(command));
        can_submit_ = false;
    }
    if (commands.size() > command_limit) {
        // 暂未交付的命令必须能在同一身份写回确认。
        const auto oldest = std::ranges::find_if(commands,[](const auto& item) { return item.receipt.state != State::queued; });
        commands.erase(oldest==commands.end() ? commands.begin() : oldest);
    }
    return receipt;
}

// 由唯一推进 Application 的线程交付命令，回执写回前保留对应槽位。
// 步骤：取走参数并保留占位 → 执行业务命令 → 写回同号结果并归还槽位。
void Host::advance_dialogue(app::Application& application, std::optional<DialogueCommand>& slot) {
    std::optional<DialogueCommand> command;
    {
        std::lock_guard lock(mutex_);
        if (!slot) return;
        command.emplace(std::move(*slot));
    }

    DialogueReceipt receipt{command->id,DialogueCommandState::rejected,command->target};
    try {
        receipt = apply_dialogue(application,*command);
    } catch (const std::exception&) {
        if (command->stored) {
            receipt = {command->id,DialogueCommandState::unavailable,command->target,0,command->stored};
            application.main_messages()->storage_failed();
        }
        // 拒绝回执只交付类别，异常正文可能包括宿主内部数据。
    }
    std::lock_guard lock(mutex_);
    if (const auto owner = clients_.find(command->client); owner!=clients_.end()) {
        auto& commands = owner->second.commands;
        const auto record = std::ranges::find_if(commands,[&](const auto& item) { return item.receipt.command==command->id; });
        if (record!=commands.end()) record->receipt = receipt;
    }
    slot.reset();
}

// 在管理线程借用已发布的不可变目录与回执，不调用业务收件方。
std::shared_ptr<const app::MailboxView> Host::mailboxes() const {
    std::lock_guard lock(mutex_);
    return mailboxes_;
}

// 在观察线程复制已发布的目录指针，目录读取没有插件调用权。
std::shared_ptr<const PluginDirectory> Host::packages() const {
    std::lock_guard lock(mutex_);
    return packages_;
}

// 在短锁内核对窗口、启动和退出状态，文件操作只持有设置自身的锁。
std::shared_ptr<app::HostSettings> Host::settings(const std::string& client, bool writing) const {
    std::lock_guard lock(mutex_);
    if (client.empty() || !clients_.contains(client)) throw std::invalid_argument("settings: management client required");
    if (writing && (exited_ || worker_.get_stop_token().stop_requested())) return {};
    return settings_;
}

// 只在业务线程拥有和使用应用服务。
// 步骤：创建应用 → 推进原工作并发布快照 → 有序关闭并报告退出。
void Host::run(std::stop_token stop, const Open& open, const Drive& drive) noexcept {
    try {
        const auto application = open();
        {
            std::lock_guard lock(mutex_);
            settings_ = application->host_settings();
        }
        if (messages_) {
            application->bind_messages(messages_,instance_,software_);
            application->main_messages()->on_reply([this](const std::string& text) { terminal_.published(text); });
        }
        auto& conversation = application->conversation();
        const auto layout = application->room().layout();
        std::shared_ptr<const app::ConversationDisplay> display;
        std::shared_ptr<const app::MailboxView> mailboxes;
        std::shared_ptr<const PluginDirectory> packages;
        while (!stop.stop_requested()) {
            if (drive) drive(*application);
            terminal_.advance(*application);
            advance_dialogue(*application,stop_command_);
            advance_dialogue(*application,pending_);
            if (application->advance_plugins() == runtime::RunResult::failed) std::fputs("A plugin notification failed.\n",stderr);
            if (auto result = conversation.advance()) terminal_.completed(*result,bool(messages_));

            const auto snapshot = application->run_status();
            const auto self = application->room().self(layout.room);
            std::optional<RoomLocation> location;
            if (self) {
                const auto place = std::ranges::find(layout.places,self->place,&app::KnownPlace::id);
                if (place != layout.places.end()) location = RoomLocation{*self,layout.version,layout.name,place->name};
            }
            const auto& visible = conversation.display();
            if (!display || display->revision != visible.revision) display = std::make_shared<const app::ConversationDisplay>(visible);
            try { mailboxes = app::sample_mailboxes(*application,mailboxes); }
            catch (const std::exception&) { mailboxes.reset(); }
            advance_plugin_actions(application->plugins(),packages);
            {
                std::lock_guard lock(mutex_);
                snapshot_ = snapshot;
                room_location_ = location;
                mailboxes_ = mailboxes;
                display_ = display;
                can_submit_ = conversation.accepting() && !conversation.busy() && !pending_ && !stop_command_ &&
                    (visible.queue.empty() || visible.queue_paused);
            }
            std::this_thread::sleep_for(interval);
        }
        shutdown(*application);
    } catch (const std::exception& error) { failed_ = true; set_stop_phase(StopPhase::failed); std::fprintf(stderr,"UI application host: %s\n",error.what()); }
    catch (...) { failed_ = true; set_stop_phase(StopPhase::failed); }
    exited_ = true;
}
}
