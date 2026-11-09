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
StateWriter::StateWriter(StateStore& store, std::string token, std::string owner)
    : store_(store), token_(std::move(token)), owner_(std::move(owner)) {}
void StateWriter::put(std::string_view kind, std::string_view id, const Json& value) { store_.put(token_, kind, id, value); }
void StateWriter::erase(std::string_view kind, std::string_view id) { store_.erase(token_, kind, id); }
void StateWriter::event(std::string type, const Json& payload, std::string run, std::string source, std::string cause, std::string conversation) {
    store_.event(owner_, std::move(type), payload, std::move(run), std::move(source), std::move(cause), std::move(conversation));
}
void StateWriter::save(std::string_view kind, std::string_view id, const Json& value, std::string type, const Json& payload, std::string run, std::string conversation) {
    store_.atomic([&] { put(kind, id, value); event(std::move(type), payload, std::move(run), "core", {}, std::move(conversation)); });
}
StateStore::StateStore(const Path& directory) : lock_(directory), db_(directory / "core.sqlite3") {
    db_.exec("CREATE TABLE IF NOT EXISTS identity(id INTEGER PRIMARY KEY CHECK(id=1),instance TEXT NOT NULL,component TEXT NOT NULL,version INTEGER NOT NULL)");
    { auto query = db_.prepare("SELECT instance FROM identity WHERE id=1"); instance_ = query.step() ? query.column(0) : identifier(); }
    db_.verify_identity(instance_, "core", 4);
    db_.exec(
        "CREATE TABLE IF NOT EXISTS objects(seq INTEGER PRIMARY KEY AUTOINCREMENT,kind TEXT NOT NULL,id TEXT NOT NULL,body TEXT NOT NULL CHECK(json_valid(body)),"
        "state TEXT GENERATED ALWAYS AS (COALESCE(json_extract(body,'$.execution'),json_extract(body,'$.state'),'')) STORED,"
        "phase TEXT GENERATED ALWAYS AS (COALESCE(json_extract(body,'$.phase'),'')) STORED,"
        "run_id TEXT GENERATED ALWAYS AS (COALESCE(json_extract(body,'$.run_id'),'')) STORED,"
        "conversation_id TEXT GENERATED ALWAYS AS (COALESCE(json_extract(body,'$.conversation'),json_extract(body,'$.route.id'),'')) STORED,"
        "activity_id TEXT GENERATED ALWAYS AS (COALESCE(json_extract(body,'$.activity'),json_extract(body,'$.input.activity'),'')) STORED,"
        "parent_id TEXT GENERATED ALWAYS AS (COALESCE(json_extract(body,'$.parent'),'')) STORED,"
        "operation_id TEXT GENERATED ALWAYS AS (COALESCE(json_extract(body,'$.active_operation'),json_extract(body,'$.operation_id'),'')) STORED,"
        "due_at INTEGER GENERATED ALWAYS AS (COALESCE(json_extract(body,'$.next_due'),json_extract(body,'$.wait_reason.until'),json_extract(body,'$.retry_at'),0)) STORED,"
        "feedback INTEGER GENERATED ALWAYS AS (kind='run' AND state IN('completed','failed','cancelled') AND activity_id<>'' AND COALESCE(json_extract(body,'$.activity_notified'),0)=0) STORED,"
        "UNIQUE(kind,id));"
        "CREATE INDEX IF NOT EXISTS objects_kind_seq ON objects(kind,seq);"
        "CREATE INDEX IF NOT EXISTS objects_kind_state_seq ON objects(kind,state,seq);"
        "CREATE INDEX IF NOT EXISTS objects_timers ON objects(kind,state,phase,due_at);"
        "CREATE INDEX IF NOT EXISTS objects_due ON objects(kind,due_at);"
        "CREATE INDEX IF NOT EXISTS objects_run_state ON objects(kind,run_id,state,seq);"
        "CREATE INDEX IF NOT EXISTS objects_parent ON objects(kind,parent_id,state);"
        "CREATE INDEX IF NOT EXISTS objects_conversation ON objects(kind,conversation_id,state);"
        "CREATE INDEX IF NOT EXISTS objects_activity ON objects(kind,activity_id,state);"
        "CREATE INDEX IF NOT EXISTS objects_operation ON objects(kind,operation_id,state);"
        "CREATE INDEX IF NOT EXISTS objects_feedback ON objects(kind,feedback,seq);"
        "CREATE TABLE IF NOT EXISTS object_resources(kind TEXT NOT NULL,id TEXT NOT NULL,resource TEXT NOT NULL,PRIMARY KEY(kind,id,resource),FOREIGN KEY(kind,id) REFERENCES objects(kind,id) ON DELETE CASCADE);"
        "CREATE INDEX IF NOT EXISTS resources_lookup ON object_resources(kind,resource,id);"
        "CREATE TRIGGER IF NOT EXISTS resources_insert AFTER INSERT ON objects BEGIN "
        "INSERT OR IGNORE INTO object_resources SELECT NEW.kind,NEW.id,value FROM json_each(COALESCE(json_extract(NEW.body,'$.descriptor.resources'),json_array(json_extract(NEW.body,'$.descriptor.resource')))) WHERE value IS NOT NULL; END;"
        "CREATE TRIGGER IF NOT EXISTS resources_update AFTER UPDATE OF body ON objects BEGIN "
        "DELETE FROM object_resources WHERE kind=NEW.kind AND id=NEW.id;"
        "INSERT OR IGNORE INTO object_resources SELECT NEW.kind,NEW.id,value FROM json_each(COALESCE(json_extract(NEW.body,'$.descriptor.resources'),json_array(json_extract(NEW.body,'$.descriptor.resource')))) WHERE value IS NOT NULL; END;"
        "CREATE TABLE IF NOT EXISTS facts(seq INTEGER PRIMARY KEY AUTOINCREMENT,id TEXT UNIQUE NOT NULL,type TEXT NOT NULL,run TEXT NOT NULL,conversation TEXT NOT NULL,body TEXT NOT NULL);"
        "CREATE INDEX IF NOT EXISTS facts_run_seq ON facts(run,seq);"
        "CREATE INDEX IF NOT EXISTS facts_conversation_seq ON facts(conversation,seq);"
        "CREATE INDEX IF NOT EXISTS facts_type_seq ON facts(type,seq);"
        "CREATE TRIGGER IF NOT EXISTS facts_immutable_update BEFORE UPDATE ON facts BEGIN SELECT RAISE(ABORT,'IMMUTABLE_EVENT'); END;"
        "CREATE TRIGGER IF NOT EXISTS facts_immutable_delete BEFORE DELETE ON facts BEGIN SELECT RAISE(ABORT,'IMMUTABLE_EVENT'); END;");
}
StateWriter StateStore::claim(std::string owner, std::initializer_list<std::string> selectors) {
    require(thread_ == std::this_thread::get_id(), "STATE_THREAD_DENIED");
    const auto token = identifier();
    for (auto selector : selectors) {
        if (selector.find('/') == std::string::npos) selector += "/*";
        const auto kind = selector.substr(0, selector.find('/'));
        for (const auto& [existing, value] : owners_) {
            (void)value;
            require(existing != selector && existing != kind + "/*" && !(selector == kind + "/*" && existing.starts_with(kind + "/")), "STATE_OWNER_ALREADY_CLAIMED");
        }
    }
    for (auto selector : selectors) { if (selector.find('/') == std::string::npos) selector += "/*"; owners_.emplace(selector, token); }
    return StateWriter(*this, token, std::move(owner));
}
void StateStore::check_owner(std::string_view token, std::string_view kind, std::string_view id) {
    require(thread_ == std::this_thread::get_id(), "STATE_THREAD_DENIED");
    const auto all = owners_.find(std::string(kind) + "/*"), exact = owners_.find(std::string(kind) + "/" + std::string(id));
    require((all != owners_.end() && all->second == token) || (exact != owners_.end() && exact->second == token), "STATE_OWNER_DENIED");
    require(transaction_depth_ != 0, "STATE_WRITE_OUTSIDE_TRANSACTION");
}
void StateStore::atomic(const std::function<void()>& action) {
    require(thread_ == std::this_thread::get_id(), "STATE_THREAD_DENIED");
    const auto before = staged_;
    if (transaction_depth_) {
        const auto name = "tx_" + std::to_string(++transaction_serial_);
        db_.exec("SAVEPOINT " + name); ++transaction_depth_;
        try { action(); db_.exec("RELEASE " + name); --transaction_depth_; }
        catch (...) { db_.exec("ROLLBACK TO " + name); db_.exec("RELEASE " + name); staged_ = before; --transaction_depth_; throw; }
        return;
    }
    Transaction transaction(db_); ++transaction_depth_;
    try {
        action(); transaction.commit(); --transaction_depth_;
        committed_.insert(staged_.begin(), staged_.end()); staged_.clear();
    } catch (...) { staged_ = before; --transaction_depth_; throw; }
}
std::vector<StateChange> StateStore::take_changes() {
    std::vector<StateChange> result;
    for (const auto& [kind, id] : committed_) result.push_back({kind, id});
    committed_.clear(); return result;
}
Json StateStore::metrics() const { return {{"queue_queries", queue_queries_}, {"queue_rows", queue_rows_}, {"queue_fullscan_steps", fullscan_steps_}, {"queue_vm_steps", vm_steps_}}; }
Json StateStore::get(std::string_view kind, std::string_view id) {
    auto query = db_.prepare("SELECT body FROM objects WHERE kind=? AND id=?"); query.bind(1, kind).bind(2, id);
    return query.step() ? Json::parse(query.column(0)) : Json{};
}
Json StateStore::query(std::string_view sql, const std::vector<std::string>& parameters) {
    auto statement = db_.prepare(sql);
    for (size_t i = 0; i < parameters.size(); ++i) statement.bind(static_cast<int>(i + 1), parameters[i]);
    Json result = Json::array(); ++queue_queries_;
    while (statement.step()) { result.push_back(Json::parse(statement.column(0))); ++queue_rows_; }
    fullscan_steps_ += statement.fullscan_steps();
    vm_steps_ += statement.vm_steps();
    return result;
}
Json StateStore::all(std::string_view kind) {
    require(kind != "run" && kind != "input" && kind != "operation" && kind != "job" && kind != "occurrence", "HISTORY_SCAN_FORBIDDEN");
    return query("SELECT body FROM objects WHERE kind=? ORDER BY seq", {std::string(kind)});
}
Json StateStore::live(std::string_view kind) { return query("SELECT body FROM objects INDEXED BY objects_kind_state_seq WHERE kind=? AND state IN('active','suspended') ORDER BY seq", {std::string(kind)}); }
Json StateStore::page(std::string_view kind, uint64_t after, size_t limit) {
    require(limit > 0 && limit <= 200, "PAGE_LIMIT");
    auto statement = db_.prepare("SELECT seq,body FROM objects WHERE kind=? AND seq>? ORDER BY seq LIMIT ?");
    statement.bind(1, kind).bind(2, std::to_string(after)).bind(3, std::to_string(limit + 1));
    Json items = Json::array(); uint64_t cursor = after; bool more = false;
    while (statement.step()) {
        if (items.size() == limit) { more = true; break; }
        cursor = std::stoull(statement.column(0)); items.push_back(Json::parse(statement.column(1)));
    }
    return {{"items", items}, {"cursor", cursor}, {"has_more", more}};
}
Json StateStore::pending_inputs(size_t limit) { return query("SELECT body FROM objects WHERE kind='input' AND state='accepted' ORDER BY seq LIMIT ?", {std::to_string(limit)}); }
size_t StateStore::pending_input_count() {
    auto statement = db_.prepare("SELECT count(*) FROM objects WHERE kind='input' AND state='accepted'"); statement.step(); return std::stoull(statement.column(0));
}
Json StateStore::active_runs() { return query("SELECT body FROM objects INDEXED BY objects_kind_state_seq WHERE kind='run' AND state IN('queued','running','waiting','paused') ORDER BY seq"); }
Json StateStore::ready_runs(int64_t now, size_t limit, int category, const Json& activities) {
    std::string filter;
    std::vector<std::string> filter_args;
    if (category == 1) return query("SELECT body FROM objects WHERE kind='run' AND state='queued' AND phase IN('action','delivery') ORDER BY seq LIMIT ?", {std::to_string(limit)});
    if (category == 2) filter = " AND activity_id=''";
    if (category == 3) {
        if (activities.empty()) return Json::array();
        filter = " AND activity_id IN(";
        for (const auto& id : activities) { if (!filter_args.empty()) filter += ','; filter += '?'; filter_args.push_back(id.get<std::string>()); }
        filter += ')';
    }
    const auto phases = category ? " AND phase NOT IN('action','delivery')" : "";
    std::vector<std::string> args = filter_args; args.push_back(std::to_string(now)); args.insert(args.end(), filter_args.begin(), filter_args.end()); args.push_back(std::to_string(limit));
    return query("SELECT body FROM (SELECT seq,body FROM objects WHERE kind='run' AND state='queued'" + filter + phases + " UNION ALL "
        "SELECT seq,body FROM objects WHERE kind='run' AND state='waiting' AND phase='wait' AND due_at>0 AND due_at<=?" + filter + ") ORDER BY seq LIMIT ?", args);
}
Json StateStore::runs_in_conversation(const std::string& id) { return query("SELECT body FROM objects WHERE kind='run' AND conversation_id=? AND state IN('queued','running','waiting','paused')", {id}); }
Json StateStore::runs_waiting_for_operation(const std::string& id) { return query("SELECT body FROM objects WHERE kind='run' AND operation_id=? AND state IN('queued','running','waiting')", {id}); }
Json StateStore::unobserved_operations() {
    return query("SELECT o.body FROM objects r JOIN objects o ON o.kind='operation' AND o.id=r.operation_id WHERE r.kind='run' AND r.state IN('waiting','queued') AND o.state IN('settled','unknown') AND json_extract(r.body,'$.observed_operation_revision')<json_extract(o.body,'$.revision') LIMIT 64");
}
Json StateStore::activity_feedback() { return query("SELECT body FROM objects WHERE kind='run' AND feedback=1 ORDER BY seq LIMIT 64"); }
Json StateStore::run_operations(const std::string& id, bool unresolved) {
    return query(unresolved ? "SELECT body FROM objects WHERE kind='operation' AND run_id=? AND state<>'settled' ORDER BY seq" :
        "SELECT body FROM objects WHERE kind='operation' AND run_id=? ORDER BY seq", {id});
}
Json StateStore::child_operations(const std::string& id) { return query("SELECT body FROM objects WHERE kind='operation' AND parent_id=? ORDER BY seq", {id}); }
Json StateStore::recoverable_operations() { return query("SELECT body FROM objects INDEXED BY objects_kind_state_seq WHERE kind='operation' AND state IN('running','job') ORDER BY seq"); }
Json StateStore::unknown_operations(const Json& resources) {
    std::string sql = "SELECT o.body FROM objects o INDEXED BY objects_kind_state_seq WHERE o.kind='operation' AND o.state='unknown' AND EXISTS (SELECT 1 FROM object_resources r WHERE r.kind=o.kind AND r.id=o.id AND r.resource IN(";
    std::vector<std::string> args;
    for (const auto& resource : resources) { if (!args.empty()) sql += ","; sql += "?"; args.push_back(resource.get<std::string>()); }
    if (args.empty()) return Json::array();
    return query(sql + "))", args);
}
Json StateStore::active_jobs(const std::string& run) {
    return run.empty() ? query("SELECT body FROM objects INDEXED BY objects_kind_state_seq WHERE kind='job' AND state IN('registered','running','interrupted') ORDER BY seq") :
        query("SELECT body FROM objects WHERE kind='job' AND run_id=? AND state<>'completed' ORDER BY seq", {run});
}
Json StateStore::pending_occurrences(const std::string& activity) {
    return activity.empty() ? query("SELECT body FROM objects WHERE kind='occurrence' AND state='pending' AND due_at<=? ORDER BY seq LIMIT 64", {std::to_string(now_ms())}) :
        query("SELECT body FROM objects WHERE kind='occurrence' AND activity_id=? AND state='pending' AND due_at<=? ORDER BY seq LIMIT 64", {activity, std::to_string(now_ms())});
}
bool StateStore::active_activity_work(const std::string& activity) {
    auto statement = db_.prepare("SELECT 1 FROM objects WHERE kind='input' AND activity_id=? AND state='accepted' UNION ALL "
        "SELECT 1 FROM objects WHERE kind='run' AND activity_id=? AND state IN('queued','running','waiting','paused') LIMIT 1");
    statement.bind(1, activity).bind(2, activity); return statement.step();
}
bool StateStore::pending_activity_occurrence(const std::string& activity) {
    auto statement = db_.prepare("SELECT 1 FROM objects WHERE kind='occurrence' AND activity_id=? AND state='pending' LIMIT 1");
    statement.bind(1, activity); return statement.step();
}
Json StateStore::due_activities(int64_t now) { return query("SELECT a.body FROM objects a JOIN objects p ON p.kind='activity_policy' AND p.id=a.id WHERE a.kind='activity' AND a.state='registered' AND a.due_at<=? AND json_extract(p.body,'$.enabled')=1 ORDER BY a.due_at LIMIT 64", {std::to_string(now)}); }
int64_t StateStore::next_wakeup() {
    auto statement = db_.prepare("SELECT MIN(value) FROM (SELECT MIN(a.due_at) AS value FROM objects a JOIN objects p ON p.kind='activity_policy' AND p.id=a.id WHERE a.kind='activity' AND a.state='registered' AND json_extract(p.body,'$.enabled')=1 UNION ALL "
        "SELECT MIN(due_at) FROM objects WHERE kind='run' AND state='waiting' AND phase='wait' AND due_at>0 UNION ALL SELECT MIN(due_at) FROM objects WHERE kind='occurrence' AND state='pending' AND due_at>0)");
    statement.step(); const auto value = statement.column(0); return value.empty() ? 0 : std::stoll(value);
}
void StateStore::put(std::string_view token, std::string_view kind, std::string_view id, const Json& value) {
    check_owner(token, kind, id);
    if (value.contains("id")) require(value.at("id").get<std::string>() == id, "STATE_ID_MISMATCH");
    auto write = db_.prepare("INSERT INTO objects(kind,id,body) VALUES(?,?,?) ON CONFLICT(kind,id) DO UPDATE SET body=excluded.body");
    write.bind(1, kind).bind(2, id).bind(3, value.dump()).step();
    staged_.emplace(std::string(kind), std::string(id));
}
void StateStore::erase(std::string_view token, std::string_view kind, std::string_view id) {
    check_owner(token, kind, id);
    auto write = db_.prepare("DELETE FROM objects WHERE kind=? AND id=?"); write.bind(1, kind).bind(2, id).step();
    staged_.emplace(std::string(kind), std::string(id));
}
void StateStore::event(std::string owner, std::string type, const Json& payload, std::string run, std::string source, std::string cause, std::string conversation) {
    require(transaction_depth_ != 0 && thread_ == std::this_thread::get_id(), "EVENT_OUTSIDE_OWNER_TRANSACTION");
    if (conversation.empty() && !run.empty()) { const auto record = get("run", run); if (!record.is_null()) conversation = record.value("conversation", std::string{}); }
    const auto id = identifier();
    Json value{{"id", id}, {"instance_id", instance_id()}, {"type", type}, {"run_id", run}, {"conversation_id", conversation},
        {"source", source}, {"owner", owner}, {"cause", cause}, {"received_at", now_ms()}, {"version", 2}, {"payload", payload}};
    auto insert = db_.prepare("INSERT INTO facts(id,type,run,conversation,body) VALUES(?,?,?,?,?)");
    insert.bind(1, id).bind(2, type).bind(3, run).bind(4, conversation).bind(5, value.dump()).step();
}
Json StateStore::events(uint64_t after, size_t limit, const std::string& run, const Json& types, const std::string& conversation) {
    require(limit > 0 && limit <= 200, "EVENT_PAGE_LIMIT");
    std::string sql = "SELECT seq,body FROM facts WHERE seq>?";
    std::vector<std::string> args{std::to_string(after)};
    if (!run.empty()) { sql += " AND run=?"; args.push_back(run); }
    if (!conversation.empty()) { sql += " AND conversation=?"; args.push_back(conversation); }
    if (!types.empty()) {
        sql += " AND type IN(";
        for (const auto& type : types) { if (sql.back() != '(') sql += ','; sql += '?'; args.push_back(type.get<std::string>()); }
        sql += ')';
    }
    sql += " ORDER BY seq LIMIT ?"; args.push_back(std::to_string(limit + 1));
    auto statement = db_.prepare(sql);
    for (size_t i = 0; i < args.size(); ++i) statement.bind(static_cast<int>(i + 1), args[i]);
    Json values = Json::array(); uint64_t cursor = after; bool more = false;
    while (statement.step()) {
        if (values.size() == limit) { more = true; break; }
        cursor = std::stoull(statement.column(0)); auto value = Json::parse(statement.column(1)); value["sequence"] = cursor; values.push_back(std::move(value));
    }
    if (!more) cursor = std::max(cursor, sequence());
    return {{"events", values}, {"cursor", cursor}, {"has_more", more}};
}
uint64_t StateStore::sequence() {
    auto query = db_.prepare("SELECT COALESCE(MAX(seq),0) FROM facts"); query.step(); return std::stoull(query.column(0));
}
}
