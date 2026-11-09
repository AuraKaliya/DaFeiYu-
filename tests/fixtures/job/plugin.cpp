#include <dafeiyu/plugin.hpp>
#include <dafeiyu/sqlite.hpp>
#include <atomic>
#include <map>
#include <thread>
using namespace dfy::sdk;
namespace {
int64_t clock_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
Json load(dfy::Database& db, const std::string& id) { auto query = db.prepare("SELECT body FROM work WHERE id=?"); query.bind(1, id); return query.step() ? Json::parse(query.column(0)) : Json{}; }
void save(dfy::Database& db, const Json& value) { auto insert = db.prepare("INSERT INTO work VALUES(?,?) ON CONFLICT(id) DO UPDATE SET body=excluded.body"); insert.bind(1, value.at("id").get<std::string>()).bind(2, value.dump()).step(); }
}
class Background : public Plugin {
    std::filesystem::path path_;
    dfy::Database db_;
    int delay_;
    std::map<uint64_t, Json> pending_;
    struct Observer { std::shared_ptr<std::atomic<bool>> done; std::jthread thread; };
    std::map<std::string, Observer> observers_;
    void observe(const std::string& job_id) {
        if (observers_.contains(job_id)) return;
        auto done = std::make_shared<std::atomic<bool>>(false);
        auto thread = std::jthread([this, job_id, done](std::stop_token stop) {
            try {
                dfy::Database db(path_);
                for (;;) {
                    if (stop.stop_requested()) break;
                    Json work;
                    { dfy::Transaction transaction(db); work = load(db, job_id);
                      if (work.at("state") != "completed" && clock_ms() >= work.at("due").get<int64_t>()) {
                          work["state"] = "completed"; work["result"] = outcome({{"value", work.at("value").get<int>() * 2}}); save(db, work);
                      }
                      transaction.commit(); }
                    if (work.at("state") == "completed") { host.job_result(job_id, 1, work.at("result")); break; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            } catch (const std::exception&) { /* Core keeps the registered job queryable when observation fails. */ }
            done->store(true);
        });
        observers_.emplace(job_id, Observer{done, std::move(thread)});
    }
public:
    static constexpr auto id = "dafeiyu.action.background";
    static constexpr auto version = "0.4.0";
    Background(Host host, const Json& config) : Plugin(host), path_(host.data_directory() / "jobs.sqlite3"), db_(path_), delay_(config.value("delay_ms", 100)) {
        db_.verify_identity(host.instance_id(), id); db_.exec("CREATE TABLE IF NOT EXISTS work(id TEXT PRIMARY KEY,body TEXT NOT NULL)");
        host.provide("action.background");
    }
    void quiesce() { for (auto& [key, observer] : observers_) { (void)key; observer.thread.request_stop(); } }
    void stop() { quiesce(); observers_.clear(); }
    void on_event(const Json& event) {
        std::erase_if(observers_, [](const auto& item) { return item.second.done->load(); });
        if (event.at("type") == "reply") {
            const auto call = event.at("call_id").get<uint64_t>();
            if (!event.at("error").is_null()) { pending_.erase(call); host.fail(call, event.at("error")); return; }
            const auto job = event.at("result"); const auto key = job.at("id").get<std::string>();
            { dfy::Transaction transaction(db_);
              if (load(db_, key).is_null()) save(db_, {{"id", key}, {"due", clock_ms() + delay_}, {"state", "running"}, {"value", pending_.at(call).at("value")}, {"result", nullptr}});
              transaction.commit(); }
            pending_.erase(call); observe(key);
            host.complete(call, {{"type", "job_started"}, {"job_id", key}}); return;
        }
        if (event.at("type") != "call") return;
        const Call call(event);
        if (call.method == "readiness") host.complete(call.id, {{"available", true}});
        else if (call.method == "execute") {
            pending_[call.id] = call.input;
            host.register_job(call.id, "job", call.context.at("operation_id"));
        } else if (call.method == "query_operation") host.complete(call.id, outcome(Json::object(), "unknown", "unknown"));
        else {
            const auto key = call.input.at("id").get<std::string>(); auto work = load(db_, key);
            if (call.method == "query_job") host.complete(call.id, work.is_null() ? Json{{"state", "interrupted"}} : work);
            else if (call.method == "reattach_job") { if (work.is_null()) throw std::runtime_error("JOB_NOT_FOUND"); observe(key); host.complete(call.id, {{"attached", true}}); }
            else if (call.method == "cancel_job") {
                if (!work.is_null()) {
                    { dfy::Transaction transaction(db_); work = load(db_, key);
                      if (work.at("state") != "completed") { work["state"] = "completed"; work["result"] = outcome(Json::object(), "none", "cancelled"); save(db_, work); }
                      transaction.commit(); }
                    host.job_result(key, 1, work.at("result"));
                }
                host.complete(call.id, {{"accepted", true}});
            } else throw std::runtime_error("METHOD_UNSUPPORTED");
        }
    }
};
DFY_PLUGIN(Background)
