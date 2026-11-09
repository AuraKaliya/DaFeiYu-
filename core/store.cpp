#include "store.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace dfy {
struct InstanceLock::Impl {
#ifdef _WIN32
    HANDLE handle{INVALID_HANDLE_VALUE};
    explicit Impl(const Path& path) {
        handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "INSTANCE_LOCK_UNAVAILABLE");
    }
    ~Impl() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
#else
    int handle{-1};
    explicit Impl(const Path& path) {
        handle = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        require(handle >= 0, "INSTANCE_LOCK_UNAVAILABLE");
        if (flock(handle, LOCK_EX | LOCK_NB) != 0) { close(handle); handle = -1; throw std::runtime_error("INSTANCE_IN_USE"); }
    }
    ~Impl() { if (handle >= 0) { flock(handle, LOCK_UN); close(handle); } }
#endif
};
InstanceLock::InstanceLock(const Path& directory) {
    std::filesystem::create_directories(directory);
    impl_ = std::make_unique<Impl>(directory / "instance.lock");
}
InstanceLock::~InstanceLock() = default;
StateStore::StateStore(const Path& directory) : lock_(directory), db_(directory / "core.sqlite3") {
    db_.exec("CREATE TABLE IF NOT EXISTS identity(id INTEGER PRIMARY KEY CHECK(id=1),instance TEXT NOT NULL,component TEXT NOT NULL,version INTEGER NOT NULL)");
    { auto query = db_.prepare("SELECT instance FROM identity WHERE id=1"); instance_ = query.step() ? query.column(0) : identifier(); }
    db_.verify_identity(instance_, "core", 3);
    db_.exec("CREATE TABLE IF NOT EXISTS objects(kind TEXT NOT NULL,id TEXT NOT NULL,body TEXT NOT NULL,PRIMARY KEY(kind,id));"
             "CREATE TABLE IF NOT EXISTS facts(seq INTEGER PRIMARY KEY AUTOINCREMENT,id TEXT UNIQUE NOT NULL,type TEXT NOT NULL,run TEXT NOT NULL,body TEXT NOT NULL);"
             "CREATE INDEX IF NOT EXISTS facts_run_seq ON facts(run,seq);"
             "CREATE TRIGGER IF NOT EXISTS facts_immutable_update BEFORE UPDATE ON facts BEGIN SELECT RAISE(ABORT,'IMMUTABLE_EVENT'); END;"
             "CREATE TRIGGER IF NOT EXISTS facts_immutable_delete BEFORE DELETE ON facts BEGIN SELECT RAISE(ABORT,'IMMUTABLE_EVENT'); END;");
}
void StateStore::atomic(const std::function<void()>& action) {
    if (transaction_depth_) { action(); return; }
    Transaction transaction(db_);
    ++transaction_depth_;
    try { action(); transaction.commit(); --transaction_depth_; }
    catch (...) { --transaction_depth_; throw; }
}
Json StateStore::get(std::string_view kind, std::string_view id) {
    auto query = db_.prepare("SELECT body FROM objects WHERE kind=? AND id=?");
    query.bind(1, kind).bind(2, id);
    return query.step() ? Json::parse(query.column(0)) : Json{};
}
Json StateStore::all(std::string_view kind) {
    auto query = db_.prepare("SELECT body FROM objects WHERE kind=? ORDER BY rowid");
    query.bind(1, kind); Json result = Json::array();
    while (query.step()) result.push_back(Json::parse(query.column(0)));
    return result;
}
void StateStore::put(std::string_view kind, std::string_view id, const Json& value) {
    require(transaction_depth_ != 0, "STATE_WRITE_OUTSIDE_TRANSACTION");
    auto write = db_.prepare("INSERT INTO objects(kind,id,body) VALUES(?,?,?) ON CONFLICT(kind,id) DO UPDATE SET body=excluded.body");
    write.bind(1, kind).bind(2, id).bind(3, value.dump()).step();
}
void StateStore::erase(std::string_view kind, std::string_view id) {
    require(transaction_depth_ != 0, "STATE_WRITE_OUTSIDE_TRANSACTION");
    auto write = db_.prepare("DELETE FROM objects WHERE kind=? AND id=?"); write.bind(1, kind).bind(2, id).step();
}
void StateStore::event(std::string type, const Json& payload, std::string run, std::string source, std::string cause) {
    require(transaction_depth_ != 0, "EVENT_OUTSIDE_TRANSACTION");
    const auto id = identifier();
    Json value{{"id", id}, {"instance_id", instance_}, {"type", type}, {"run_id", run}, {"source", source},
        {"cause", cause}, {"received_at", now_ms()}, {"version", 1}, {"payload", payload}};
    auto insert = db_.prepare("INSERT INTO facts(id,type,run,body) VALUES(?,?,?,?)");
    insert.bind(1, id).bind(2, type).bind(3, run).bind(4, value.dump()).step();
}
void StateStore::save(std::string_view kind, std::string_view id, const Json& value, std::string type, const Json& payload, std::string run) {
    atomic([&] { put(kind, id, value); event(std::move(type), payload, std::move(run)); });
}
Json StateStore::events(uint64_t after, size_t limit, const std::string& run, const Json& types) {
    require(limit > 0 && limit <= 200, "EVENT_PAGE_LIMIT");
    auto query = db_.prepare("SELECT seq,body FROM facts WHERE seq>? ORDER BY seq LIMIT 1000");
    query.bind(1, std::to_string(after)); Json values = Json::array(); uint64_t cursor = after;
    while (query.step()) {
        cursor = std::stoull(query.column(0)); auto value = Json::parse(query.column(1));
        if ((!run.empty() && value.at("run_id") != run) || (!types.empty() && !includes(types, value.at("type")))) continue;
        value["sequence"] = cursor; values.push_back(std::move(value));
        if (values.size() == limit) break;
    }
    return {{"events", values}, {"cursor", cursor}};
}
uint64_t StateStore::sequence() {
    auto query = db_.prepare("SELECT COALESCE(MAX(seq),0) FROM facts"); query.step(); return std::stoull(query.column(0));
}
}
