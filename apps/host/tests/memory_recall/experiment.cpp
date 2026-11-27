#include <winsqlite/winsqlite3.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

// 将 SQLite 错误报告给离线验收入口。
void check(int result, sqlite3* db) {
    if (result != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
}

// 从测试资源读取完整 SQL。
std::string read_sql(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open fixture: " + file.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// 固定实验失败的条件及所在用例。
void require(bool passed, const std::string& label) {
    if (!passed) throw std::runtime_error(label);
}

struct Query {
    const char* agent = "main";
    const char* task = "review";
    const char* audience = "owner";
    const char* layer = "preference";
    const char* term = "周报";
    int as_of = 30;
    int per_store = 3;
    std::size_t bytes = 256;
};
struct Result {
    std::vector<std::string> candidates;
    std::vector<std::string> events;
    std::string context;
};

// 以整条证据装入一个全局 UTF-8 字节预算，保留出处并跳过放不下的候选。
// 步骤：绑定本次查询 → 读取已授权候选 → 按原事件去重并计算序列化后的完整大小。
Result recall(sqlite3* db, const std::string& sql, const Query& query) {
    sqlite3_stmt* raw = nullptr;
    check(sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr), db);
    Statement statement(raw, sqlite3_finalize);
    const auto bind = [&](const char* name, const char* value) {
        check(sqlite3_bind_text(raw, sqlite3_bind_parameter_index(raw, name), value, -1, SQLITE_TRANSIENT), db);
    };
    bind(":agent", query.agent);
    bind(":task", query.task);
    bind(":audience", query.audience);
    bind(":layer", query.layer);
    bind(":term", query.term);
    check(sqlite3_bind_int(raw, sqlite3_bind_parameter_index(raw, ":as_of"), query.as_of), db);
    check(sqlite3_bind_int(raw, sqlite3_bind_parameter_index(raw, ":per_store"), query.per_store), db);
    Result result;
    std::set<std::string> selected;
    int status;
    while ((status = sqlite3_step(raw)) == SQLITE_ROW) {
        const auto column = [&](int i) {
            const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(raw, i));
            return std::string(text, sqlite3_column_bytes(raw, i));
        };
        const std::string event = column(2);
        result.candidates.push_back(column(0) + "/" + column(1));
        if (selected.contains(event)) continue;
        const auto line = Json{{"store",column(0)}, {"id",column(1)}, {"event",event},
            {"revision",sqlite3_column_int(raw,3)}, {"text",column(4)}}.dump() + "\n";
        if (line.size() > query.bytes - result.context.size()) continue;
        result.context += line;
        selected.insert(event);
        result.events.push_back(event);
    }
    if (status != SQLITE_DONE) check(status, db);
    return result;
}

// 逐批引入干扰库，用同一份问题和预算核对授权候选与最终上下文。
// 步骤：增加材料并计时 → 对比时点与来源 → 复查角色、受众、委托和字节边界。
void verify(sqlite3* db, const std::string& sql) {
    Query q;
    std::string baseline;
    for (int batch = 0; batch <= 4; ++batch) {
        const auto insert = "INSERT INTO entries SELECT store,id,event,revision,layer,topic,body,since,until,copied "
            "FROM batches WHERE batch=" + std::to_string(batch);
        check(sqlite3_exec(db, insert.c_str(), nullptr, nullptr, nullptr), db);
        const auto start = std::chrono::steady_clock::now();
        const auto result = recall(db, sql, q);
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start);
        std::cout << "batch=" << batch << " candidates=" << result.candidates.size() << " selected=" << result.events.size()
            << " bytes=" << result.context.size() << "/" << q.bytes << " elapsed_us=" << us.count() << '\n';
        if (batch == 0) baseline = result.context;
        if (batch < 4) {
            require(result.events == std::vector<std::string>{"E2"} && result.context == baseline, "distractor changed evidence");
            const std::vector<std::string> expected = batch < 2 ? std::vector<std::string>{"personal/now"} :
                std::vector<std::string>{"personal/now", "received/copy-a"};
            require(result.candidates == expected, "unauthorized, stale or duplicate local candidate");
        } else {
            require(result.events == std::vector<std::string>{"E3","E2"}, "independent disagreement lost");
        }
    }
    const auto first = Json::parse(baseline);
    require(first.at("text") == "先写结论。" && first.at("revision") == 2 && first.at("store") == "personal",
        "changed source, revision or meaning");
    const auto expect = [&](Query query, std::vector<std::string> ids, const char* label) {
        const auto result = recall(db, sql, query);
        require(result.events == ids && result.context.size() <= query.bytes, label);
        if (ids.empty()) require(result.candidates.empty(), label);
    };
    q.as_of = 19;
    expect(q, {"E1"}, "past interpretation");
    q.as_of = 20;
    expect(q, {"E2"}, "revision boundary");
    q.layer = "quote";
    expect(q, {"Q1"}, "original quote");
    q.layer = "preference";
    q.term = "结论";
    expect(q, {"E2"}, "word in body");
    q.term = "未提及";
    expect(q, {}, "unrelated question");
    q = {};
    q.term = "";
    expect(q, {}, "empty question");
    expect({"avatar","reply","public"}, {"P1"}, "public persona");
    expect({"avatar","reply","owner"}, {}, "wrong audience");
    expect({"avatar","reply","public","quote"}, {}, "private original");
    expect({"business-a","service","internal"}, {"B1"}, "independent business");
    expect({"one-shot","job-7","internal","knowledge","交付"}, {"T1"}, "task brief");
    expect({"one-shot","job-8","internal","knowledge","交付"}, {}, "different task");
    expect({"main","review","owner","knowledge","归档",29}, {"K1"}, "previous knowledge");
    expect({"main","review","owner","knowledge","归档",30}, {"K2"}, "current knowledge");
    const auto travel = recall(db, sql, {"main","travel","owner","quote","往返行程",30,3,384});
    require(travel.events == std::vector<std::string>{"F3","F2","F1"} &&
        travel.context.find("预订编号 DFY-42。") != std::string::npos &&
        travel.context.find("去程座位 3A。") != std::string::npos &&
        travel.context.find("回程座位 7C。") != std::string::npos, "incomplete round-trip evidence");
    q = {};
    q.as_of = 20;
    q.bytes = baseline.size();
    require(recall(db, sql, q).context == baseline, "exact UTF-8 budget");
    --q.bytes;
    require(recall(db, sql, q).context.empty(), "byte underflow");
    q = {};
    q.per_store = 1;
    require(recall(db, sql, q).candidates == std::vector<std::string>{"received/independent","personal/now"},
        "per-store limit");
    q.bytes = 0;
    require(recall(db, sql, q).context.empty(), "zero budget");
    q.bytes = baseline.size();
    require(recall(db, sql, q).context == baseline, "oversize first item hid fitting evidence");

    check(sqlite3_exec(db, "DELETE FROM grants WHERE agent='avatar'", nullptr, nullptr, nullptr), db);
    expect({"avatar","reply","public"}, {}, "revoked grant still visible");
    expect({"business-a","service","internal"}, {"B1"}, "unrelated grant lost");
}

// 在系统 SQLite 的临时逻辑库内回放合成材料。
int run(const std::filesystem::path& directory) {
    sqlite3* raw = nullptr;
    const auto status = sqlite3_open(":memory:", &raw);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    check(status, raw);
    check(sqlite3_exec(raw, read_sql(directory/"corpus.sql").c_str(), nullptr, nullptr, nullptr), raw);
    verify(raw, read_sql(directory/"search.sql"));
    return 0;
}
}

// CTest 提供资源目录并接收失败原因。
int wmain(int argc, wchar_t** argv) {
    try {
        return argc == 2 ? run(argv[1]) : 2;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
