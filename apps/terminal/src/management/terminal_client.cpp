#include "terminal_client.hpp"
#include "plugin_commands.hpp"
#include "chat.hpp"
#include <windows.h>
#include <nlohmann/json.hpp>
#include <ostream>

namespace dfy::management {
using Json = nlohmann::json;
using namespace std::chrono_literals;

// 仅在原客户端与同一命令身份上重试短暂的管道断包。
// 步骤：固定原命令与游标 → 轮询同号回包 → 有界重试临时断连或交付确切错误。
Json terminal_exchange(const TerminalTransport& send, const std::string& client, std::uint64_t id, Json body) {
    body["id"] = id;
    const Json request{{"op","terminal"},{"client",client},{"body",std::move(body)}};
    unsigned failures = 0;
    for (;;) {
        try {
            auto result = send(request);
            if (!result.is_null()) {
                if (result.contains("error")) throw std::runtime_error(result.at("error").get<std::string>());
                return result;
            }
        } catch (const std::system_error& error) {
            const auto code = error.code().value();
            const bool disconnected = code==ERROR_BROKEN_PIPE || code==ERROR_NO_DATA || code==ERROR_PIPE_NOT_CONNECTED;
            if (++failures>2 || error.code().category()!=std::system_category() || (!disconnected && code!=ERROR_SEM_TIMEOUT)) throw;
        }
        std::this_thread::sleep_for(5ms);
    }
}

namespace {
// 安装、诊断和显式工具委托按发起终端的位置解析本地路径，再交给常驻宿主。
std::string terminal_paths(const std::string& line) {
    if (!line.starts_with("/diagnostic") && !line.starts_with("/plugins") && !line.starts_with("/run")) return line;
    const auto words = app::command_words(line);
    if (words.size()==5 && (words[0]=="/run" || words[0]=="/run-confirmed") && words[3]=="--workspace" && !words[4].empty()) {
        const auto path = std::filesystem::absolute(std::u8string(words[4].begin(),words[4].end())).u8string();
        return words[0]+" "+words[1]+" \""+words[2]+"\" --workspace \""+std::string(path.begin(),path.end())+"\"";
    }
    if (words.size()!=3 || !((words[0]=="/diagnostic" && words[1]=="export") || (words[0]=="/plugins" && words[1]=="install"))) return line;
    const auto path = std::filesystem::absolute(std::u8string(words[2].begin(),words[2].end())).u8string();
    return words[0]+" "+words[1]+" \""+std::string(path.begin(),path.end())+"\"";
}
}

// 在原客户端空间执行命令与补读，批处理 EOF 可收取本终端已发问答。
// 步骤：建立输出水位 → 交错轮询输入与后台 → 实际打印后确认 ASK → 分离终端。
int resident_chat(const std::filesystem::path& profile, const ResidentConnection& binding, platform::LineInput& input,
    std::ostream& output, std::ostream& errors, app::SoftwareLog* log) {
    struct Detach {
        const std::filesystem::path& profile;
        const std::string& client;
        app::SoftwareLog* log;
        // 尽力归还观察身份，传输失败保留原业务和错误结果。
        ~Detach() {
            try {
                (void)resident_request(profile,{{"op","disconnect"},{"client",client}});
                if (log) log->record(app::SoftwareEvent::client_disconnected,app::SoftwareResult::ok);
            } catch (...) { if (log) log->record(app::SoftwareEvent::client_disconnected,app::SoftwareResult::failed); }
        }
    } detach{profile,binding.client,log};
    std::uint64_t sequence = 0, own_turn = 0, printed_ask = 0;
    const TerminalTransport send = [&](const Json& request) { return resident_request(profile,request); };
    const auto call = [&](Json request) { return terminal_exchange(send,binding.client,++sequence,std::move(request)); };
    const bool interactive = input.interactive_console();
    const auto opening = call({{"action","open"},{"interactive",interactive}});
    auto cursor = opening.at("cursor").get<std::uint64_t>();
    errors << opening.at("err").get_ref<const std::string&>() << std::flush;
    output << "DaFeiYu\nEnter a message, /status, /clear, /cancel or /exit. Local commands remain available while waiting.\n" << std::flush;
    auto poll_at = std::chrono::steady_clock::now();
    std::optional<std::chrono::steady_clock::time_point> storage_deadline;
    bool eof = false;
    while (output && errors) {
        if (std::chrono::steady_clock::now()>=poll_at) {
            const auto reply = call({{"action","read"},{"after",cursor}});
            output << reply.at("out").get_ref<const std::string&>() << std::flush;
            errors << reply.at("err").get_ref<const std::string&>() << std::flush;
            if (!output || !errors) return 1;
            cursor = reply.at("cursor");
            if (interactive && reply.contains("ask") && reply.at("ask").at("id")!=printed_ask) {
                const auto& ask = reply.at("ask");
                output << ask.at("text").get_ref<const std::string&>() << '\n';
                for (const auto& choice : ask.at("choices")) output << "  " << choice.get_ref<const std::string&>() << '\n';
                const auto id = ask.at("id").get<std::uint64_t>();
                output << "/ask answer " << id << " \"ANSWER\"\n" << std::flush;
                if (output && call({{"action","presented"},{"ask",id}}).at("presented")==true) printed_ask = id;
            }
            const bool more = reply.at("more");
            if (eof && !more && (!own_turn || reply.at("turn")!=own_turn || reply.at("busy")==false || reply.at("confirming")==true)) {
                if (!own_turn || reply.at("turn")!=own_turn || !reply.contains("pendingDelivery")) break;
                const auto now = std::chrono::steady_clock::now();
                if (!storage_deadline) storage_deadline = now+4s;
                if (now>=*storage_deadline) {
                    errors << "Reply is waiting for storage. Delivery " << reply.at("pendingDelivery").get<std::string>() << '\n';
                    return 1;
                }
            }
            poll_at = std::chrono::steady_clock::now()+(more ? 0ms : 100ms);
        }
        if (!eof) {
            auto read = input.poll();
            if (read.state==platform::InputState::line) {
                if (app::terminal_line(read.line)=="/exit") break;
                std::string line;
                try { line = terminal_paths(read.line); }
                catch (const std::exception& error) { errors << error.what() << '\n' << std::flush; continue; }
                const auto reply = call({{"action","line"},{"text",std::move(line)}});
                output << reply.at("out").get_ref<const std::string&>() << std::flush;
                errors << reply.at("err").get_ref<const std::string&>() << std::flush;
                if (reply.at("started")==true) own_turn = reply.at("turn");
                poll_at = std::chrono::steady_clock::now();
            } else if (read.state==platform::InputState::end) {
                eof = true;
                poll_at = std::chrono::steady_clock::now();
            } else if (read.state==platform::InputState::failed) {
                errors << "Input failed: " << read.error << '\n';
                return 1;
            }
        }
        input.advance();
        std::this_thread::sleep_for(5ms);
    }
    input.close();
    return output && errors ? 0 : 1;
}
}
