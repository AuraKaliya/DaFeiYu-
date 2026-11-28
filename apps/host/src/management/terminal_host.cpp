#include "terminal_host.hpp"
#include "application.hpp"
#include "application_commands.hpp"
#include "main_messages.hpp"
#include "chat.hpp"
#include "plugin_commands.hpp"
#include <algorithm>
#include <sstream>

namespace dfy::management {
using Json = nlohmann::json;

// 相同客户端编号只执行一次，未完成的工作由短轮询查回。
// 步骤：核对命令契约 → 复用原结果或拒绝冲突 → 在容量内登记实际工作。
Json TerminalHost::exchange(const std::string& client, const Json& request) {
    const auto& id = request.at("id");
    const auto& action = request.at("action");
    const bool valid = (action=="open" && request.at("interactive").is_boolean()) ||
        (action=="read" && request.at("after").is_number_unsigned()) ||
        (action=="presented" && request.at("ask").is_number_unsigned()) ||
        (action=="line" && request.at("text").get_ref<const std::string&>().size()<=app::dialogue_text_limit);
    if (!id.is_number_unsigned() || id==0 || !valid || request.size()!=3) throw std::invalid_argument("invalid terminal command");

    std::lock_guard lock(mutex_);
    auto& owner = clients_[client];
    owner.seen = std::chrono::steady_clock::now();
    if (owner.last && id<=owner.last->request.at("id")) {
        if (request!=owner.last->request) throw std::invalid_argument("terminal command identity conflict");
        if (owner.last->reply.contains("ask")) owner.offered_ask = owner.last->reply.at("ask").at("id");
        return owner.last->reply;
    }
    if ((owner.last && owner.last->reply.is_null()) || pending_.size()>=32) throw std::invalid_argument("terminal command busy");
    if (action=="presented" && (!owner.interactive || !owner.offered_ask || request.at("ask")!=owner.offered_ask))
        throw std::invalid_argument("terminal question was not offered to this client");
    if (action=="open") {
        owner.interactive = request.at("interactive");
        owner.offered_ask = 0;
    }
    auto job = std::make_shared<Job>(Job{request,nullptr,client,owner.interactive,owner.offered_ask});
    pending_.push_back(job);
    owner.last = std::move(job);
    return nullptr;
}

// 收回已离开终端的呈现资格与原命令查询空间。
void TerminalHost::disconnect(const std::string& client) {
    std::lock_guard lock(mutex_);
    clients_.erase(client);
}

// 为实际输出分配游标，保留有界的近期正文与原接收方。
void TerminalHost::append_output(std::string out, std::string err, std::string client) {
    if (out.empty() && err.empty()) return;
    outputs_.push_back({++cursor_,std::move(out),std::move(err),std::move(client)});
    output_bytes_ += outputs_.back().out.size()+outputs_.back().err.size();
    while (outputs_.size()>16 || (outputs_.size()>1 && output_bytes_>app::dialogue_display_byte_limit)) {
        output_bytes_ -= outputs_.front().out.size()+outputs_.front().err.size();
        outputs_.pop_front();
    }
}

// 将持久渠道已经确认的发言发布给当前主聊天的各终端。
void TerminalHost::published(const std::string& text) { append_output(text+"\n",{}); }

// 只将命令型回合的内部结果交给发起终端，共享聊天沿原投递显示。
// 步骤：核对原回合与受众 → 格式化终态诊断或命令输出 → 发布给对应终端。
void TerminalHost::completed(const app::CompletedTurn& turn, bool persistent) {
    const bool private_result = persistent && !turn.user_facing;
    if (private_result && command_run_!=agent::RunId{turn.status.session,turn.status.turn}) return;
    std::ostringstream out,err;
    app::write_chat_result(turn,out,err,!persistent || private_result);
    append_output(std::move(out).str(),std::move(err).str(),private_result ? command_client_ : std::string{});
}

// 在同一主会话处理终端输入，回包中的 ASK 仍要等终端实际打印后确认。
// 步骤：执行原生命令或补读输出 → 采样主回合及等待 → 附上仍待呈现的提问。
Json TerminalHost::execute(app::Application& application, const Job& job) {
    const auto& request = job.request;
    const auto& action = request.at("action");
    auto& conversation = application.conversation();
    std::ostringstream out, err;
    Json reply{{"cursor",cursor_},{"more",false},{"started",false}};
    if (action=="line") {
        const auto before = conversation.status();
        app::ChatController controller(conversation,out,err,[&](std::string_view command) {
            if (command.substr(0,command.find_first_of(" \t"))=="/resume") {
                const auto args=app::command_words(command);
                if (args.size()!=3 || !application.main_messages()) throw std::invalid_argument("Usage: /resume TASK_REF \"MESSAGE\"");
                application.main_messages()->terminal(job.client+"/"+request.at("id").dump(),args[2],args[1],true);
                return true;
            }
            if (command.substr(0,command.find_first_of(" \t"))=="/ask") {
                const auto args = app::command_words(command);
                if (args.size()==4 && (!job.interactive || !job.offered_ask || args[2]!=std::to_string(job.offered_ask)))
                    throw std::invalid_argument("ASK was not presented on this terminal");
            }
            return app::application_command(application,command,out,err);
        },
            [&](const std::string& text) {
                if (!application.main_messages()) return application.submit_dialogue(text);
                application.main_messages()->terminal(job.client+"/"+request.at("id").dump(),text);
                return application.conversation().status().value();
            });
        controller.line(request.at("text").get<std::string>(),app::ChatExit::detach);
        const auto after = conversation.status();
        if (after && (!before || before->turn!=after->turn)) {
            command_run_ = {after->session,after->turn};
            command_client_ = job.client;
            reply["started"] = true;
        }
    } else if (action=="open") {
        const auto& error = application.plugins().directory_error();
        if (!error.empty()) err << "Plugin directory unavailable; chat remains available: " << error << '\n';
    } else if (action=="presented") {
        reply["presented"] = application.answer_console_ask(request.at("ask").get<std::uint64_t>(),{});
    } else {
        const auto after = request.at("after").get<std::uint64_t>();
        if (!outputs_.empty() && after<outputs_.front().cursor-1) err << "Some earlier terminal output is no longer retained.\n";
        const auto next = std::ranges::find_if(outputs_,[&](const auto& row) {
            return row.cursor>after && (row.client.empty() || row.client==job.client);
        });
        if (next!=outputs_.end()) {
            out << next->out;
            err << next->err;
            reply["cursor"] = next->cursor;
            reply["more"] = next->cursor<cursor_;
        }
    }
    const auto status = conversation.status();
    if (action=="read" && status && application.main_messages()) {
        const auto delivery = application.main_messages()->pending_reply({status->session,status->turn});
        if (!delivery.empty()) reply["pendingDelivery"] = delivery;
    }
    reply.update(Json{{"out",std::move(out).str()},{"err",std::move(err).str()},{"turn",status ? status->turn : 0},
        {"busy",conversation.busy()},{"confirming",status && status->waiting_confirmations}});
    if (const auto ask = application.console_ask(); job.interactive && ask)
        reply["ask"] = {{"id",ask->first},{"text",ask->second.question},{"choices",ask->second.choices}};
    return reply;
}

// 取走一项已接纳请求，调用期间允许管道继续回答状态、退出及其他客户端。
// 步骤：采样近期可交互终端 → 移交原作业 → 在锁外执行 → 写回同号结果。
void TerminalHost::advance(app::Application& application) {
    std::shared_ptr<Job> job;
    {
        std::lock_guard lock(mutex_);
        const auto cutoff = std::chrono::steady_clock::now()-std::chrono::seconds(5);
        application.set_console_ask_available(std::ranges::any_of(clients_,[&](const auto& item) {
            return item.second.interactive && item.second.seen>cutoff;
        }));
        if (pending_.empty()) return;
        job = std::move(pending_.front());
        pending_.pop_front();
    }
    Json reply;
    try { reply = execute(application,*job); }
    catch (const std::exception& error) { reply = {{"error",error.what()}}; }
    std::lock_guard lock(mutex_);
    job->reply = std::move(reply);
}
}
