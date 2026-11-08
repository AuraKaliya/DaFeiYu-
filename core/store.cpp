#include <dafeiyu/core.hpp>
#include <iomanip>
#include <random>
#include <sstream>
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
namespace {
std::string identifier() {
    std::random_device random;
    std::ostringstream result;
    for (int i = 0; i < 4; ++i) result << std::hex << std::setw(8) << std::setfill('0') << random();
    return result.str();
}
std::string now() {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}
}
struct InstanceLock::Impl {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit Impl(const Path& file) {
        handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("INSTANCE_IN_USE_OR_LOCK_UNAVAILABLE");
    }
    ~Impl() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
#else
    int handle = -1;
    explicit Impl(const Path& file) {
        handle = open(file.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (handle < 0) throw std::runtime_error("INSTANCE_LOCK_UNAVAILABLE");
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
Store::Store(const Path& directory) : lock_(directory), db_(directory / "core.sqlite3") {
    db_.exec("CREATE TABLE IF NOT EXISTS identity (id INTEGER PRIMARY KEY CHECK(id=1), instance TEXT NOT NULL, component TEXT NOT NULL, version INTEGER NOT NULL)");
    {
        auto read = db_.prepare("SELECT instance FROM identity WHERE id=1");
        instance_ = read.step() ? read.column(0) : identifier();
    }
    db_.verify_identity(instance_, "core");
    db_.exec("CREATE TABLE IF NOT EXISTS runs (id TEXT PRIMARY KEY, request_id TEXT NOT NULL UNIQUE, body TEXT NOT NULL);"
             "CREATE TABLE IF NOT EXISTS events (seq INTEGER PRIMARY KEY AUTOINCREMENT, run_id TEXT NOT NULL REFERENCES runs(id), type TEXT NOT NULL, payload TEXT NOT NULL, at_ms TEXT NOT NULL);"
             "CREATE TRIGGER IF NOT EXISTS events_no_update BEFORE UPDATE ON events BEGIN SELECT RAISE(ABORT,'IMMUTABLE_EVENT'); END;"
             "CREATE TRIGGER IF NOT EXISTS events_no_delete BEFORE DELETE ON events BEGIN SELECT RAISE(ABORT,'IMMUTABLE_EVENT'); END;");
}
Json Store::find(const std::string& request_id) {
    auto query = db_.prepare("SELECT body FROM runs WHERE request_id=?");
    query.bind(1, request_id);
    return query.step() ? Json::parse(query.column(0)) : Json{};
}
Json Store::create(const std::string& request_id, const Json& inbound, const Json& bindings, const std::string& foundation) {
    Json run{{"schema_version", 1}, {"instance_id", instance_}, {"run_id", identifier()}, {"request_id", request_id},
        {"input", inbound}, {"bindings", bindings}, {"foundation", foundation}, {"intent_revision", 1},
        {"phase", "memory"}, {"execution", "queued"}, {"goal", "uncertain"}, {"delivery", "none"},
        {"turn", 0}, {"memory", nullptr}, {"results", Json::array()}, {"operations", Json::array()}, {"error", nullptr}};
    Transaction tx(db_);
    auto insert = db_.prepare("INSERT INTO runs VALUES(?,?,?)");
    insert.bind(1, run.at("run_id").get<std::string>()).bind(2, request_id).bind(3, run.dump()).step();
    auto event = db_.prepare("INSERT INTO events(run_id,type,payload,at_ms) VALUES(?,'run.accepted',?,?)");
    event.bind(1, run.at("run_id").get<std::string>()).bind(2, inbound.dump()).bind(3, now()).step();
    tx.commit();
    return run;
}
void Store::commit(const Json& run, const std::string& type, const Json& payload) {
    Transaction tx(db_);
    auto update = db_.prepare("UPDATE runs SET body=? WHERE id=?");
    update.bind(1, run.dump()).bind(2, run.at("run_id").get<std::string>()).step();
    auto append = db_.prepare("INSERT INTO events(run_id,type,payload,at_ms) VALUES(?,?,?,?)");
    append.bind(1, run.at("run_id").get<std::string>()).bind(2, type).bind(3, payload.dump()).bind(4, now()).step();
    tx.commit();
}
Json Store::events(const std::string& request_id) {
    const auto run = find(request_id);
    if (run.is_null()) throw std::runtime_error("REQUEST_NOT_FOUND");
    auto query = db_.prepare("SELECT seq,type,payload,at_ms FROM events WHERE run_id=? ORDER BY seq");
    query.bind(1, run.at("run_id").get<std::string>());
    Json result = Json::array();
    while (query.step()) result.push_back({{"sequence", std::stoll(query.column(0))}, {"type", query.column(1)},
        {"payload", Json::parse(query.column(2))}, {"at_unix_ms", query.column(3)}});
    return result;
}
}
