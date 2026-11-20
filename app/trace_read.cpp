#include "trace_read.hpp"
#include "trace_commands.hpp"
#include <charconv>
#include <fstream>
#include <set>
#include <nlohmann/json.hpp>

namespace dfy::app {
namespace {
using Json = nlohmann::json;
constexpr std::size_t maximum_bytes = 512*1024*1024, maximum_line = 16*1024*1024;

// 有界读取单条日志，遇到非法正文或不连续序号时指出首个异常位置。
Json parse_record(std::string_view line, std::uint64_t sequence) {
    if (line.size() > maximum_line) throw std::length_error("trace: record exceeds 16 MiB");
    try {
        std::vector<std::set<std::string>> objects;
        // 记录嵌套对象的键，避免重复字段覆盖原身份或正文。
        auto value = Json::parse(line,[&](int depth,Json::parse_event_t event,const Json& item) {
            if (depth > 48) throw std::invalid_argument("trace: record nested too deeply");
            if (event == Json::parse_event_t::object_start) objects.emplace_back();
            else if (event == Json::parse_event_t::object_end) objects.pop_back();
            else if (event == Json::parse_event_t::key && !objects.back().insert(item.get<std::string>()).second)
                throw std::invalid_argument("trace: duplicate field");
            return true;
        });
        if (value.at("schema") != 1 || !value.at("seq").is_number_unsigned() ||
            value.at("seq") != sequence || !value.at("kind").is_string() ||
            (value.at("kind") == "trace.open") != (sequence == 1))
            throw std::invalid_argument("trace: unsupported schema or non-contiguous identity");
        return value;
    } catch (const std::exception&) { throw std::invalid_argument("trace: invalid record at line "+std::to_string(sequence)); }
}

// 列出回合和玩家消息锚点。正文按指定回合或原日志展开。
void summarize(const Json& record, std::ostream& output) {
    const auto& kind = record.at("kind");
    if (kind == "run.exited")
        output << "session " << record.at("session") << " run " << record.at("run") << ' ' << record.at("source") << ' ' << record.at("result") << '\n';
    if (kind != "mail.received") return;
    const auto& category = record.at("category");
    const auto& body = record.at("body");
    if (category == "mc.partner.event")
        output << "game event " << body.at("event") << '\n';
    if (category == "mc.partner.turn" && body.contains("session"))
        output << "game order " << body.at("order") << " -> child session " << body.at("session")
            << " request " << body.at("request") << '\n';
}
}

// 逐条交付已校验的记录，未落完的一行不作为已经发生的事件。
// 步骤：限定文件容量 → 校验首行及顺序 → 报告完整行或截尾。
bool read_trace(const std::filesystem::path& file, const std::function<void(const Json&)>& visit) {
    if (std::filesystem::file_size(file) > maximum_bytes) throw std::length_error("trace: file exceeds 512 MiB");
    std::ifstream input(file,std::ios::binary);
    if (!input) throw std::runtime_error("trace: cannot open file");
    std::string line;
    std::uint64_t sequence = 0;
    bool closed = false;
    while (std::getline(input,line)) {
        if (closed) throw std::invalid_argument("trace: record follows the closing event");
        if (input.eof() && sequence) return false;
        auto record = parse_record(line,++sequence);
        if (input.eof()) throw std::invalid_argument("trace: incomplete log header");
        closed = record.at("kind") == "trace.closed";
        visit(record);
    }
    if (!input.eof() || !sequence) throw std::runtime_error("trace: cannot read a log header");
    return closed;
}

// 核对原会话与回合，不以日志内顺序推测归属。
bool matches_run(const Json& record, agent::RunId run) {
    return record.contains("session") && record.contains("run") && record.at("session") == run.session && record.at("run") == run.sequence;
}

// 从终端参数读取正整数，拒绝部分匹配或溢出。
runtime::DomainId trace_id(const std::filesystem::path& value) {
    const auto text = value.string();
    runtime::DomainId id = 0;
    const auto result = std::from_chars(text.data(),text.data()+text.size(),id);
    if (result.ec != std::errc{} || result.ptr != text.data()+text.size() || !id)
        throw std::invalid_argument("trace: positive numeric identity required");
    return id;
}

// 展示指定运行中的原记录及按子请求关联的游戏信箱事件。
// 步骤：扫描有序数据 → 按身份输出原记录 → 提示记录是否缺少完整退出。
void review_trace(const std::filesystem::path& file, std::optional<agent::RunId> run, std::ostream& output) {
    runtime::DomainId child_request = 0;
    bool found = false, ended = false;
    const auto complete = read_trace(file,[&](const Json& record) {
        if (!run) { summarize(record,output); return; }
        const auto& kind = record.at("kind");
        if (matches_run(record,*run)) {
            found = true;
            ended |= kind == "run.exited";
            if (kind == "subagent.accepted") child_request = record.at("request").get<runtime::DomainId>();
            output << record.dump() << '\n';
        } else if (child_request && kind == "mail.received") {
            const auto& body = record.at("body");
            if (body.contains("session") && body.contains("request") && body.at("session") == run->session && body.at("request") == child_request)
                output << record.dump() << '\n';
        }
    });
    if (run && !found) throw std::invalid_argument("trace: no such run");
    if (!complete || (run && !ended)) output << "Trace is incomplete.\n";
}
}
