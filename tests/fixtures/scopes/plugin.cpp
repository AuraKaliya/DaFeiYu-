#include <dafeiyu/plugin.hpp>
#include <dafeiyu/contracts.hpp>
#include <dafeiyu/sqlite.hpp>
#include <thread>
using namespace dfy::sdk;
class ScopeProbe : public Plugin {
    Json config_;
    dfy::Database db_;
    std::string scope_;
    uint64_t pending_{};
    std::vector<std::string> registrations_;
    void record(const Json& event) {
        auto insert = db_.prepare("INSERT INTO observations(body) VALUES(?)"); insert.bind(1, event.dump()).step();
    }
    void activate(const std::string& scope) {
        scope_ = scope;
        host.register_scoped(scope_, "context", "context", {{"service", "fixture.context.extra"}});
        host.register_scoped(scope_, "number", "action", {{"action", "fixture.number"}});
        host.register_scoped(scope_, "text", "action", {{"action", "fixture.text"}});
        host.request_scoped(scope_, "snapshot", "memory", "snapshot", Json::object());
        if (config_.value("probe_denied", false)) host.request_scoped(scope_, "denied", "missing", "snapshot", Json::object());
        host.subscribe(scope_, "facts", Json::array({"run.accepted", "run.completed", "operation.settled"}));
    }
    void collect_done() { if (pending_) { host.complete(pending_, {{"items", Json::array()}}); pending_ = 0; } }
public:
    static constexpr auto id = "test.scopes";
    static constexpr auto version = "0.5.0";
    ScopeProbe(Host host, Json config) : Plugin(host), config_(std::move(config)), db_(host.data_directory() / "observations.sqlite3") {
        db_.verify_identity(host.instance_id(), id);
        db_.exec("CREATE TABLE IF NOT EXISTS observations(seq INTEGER PRIMARY KEY,body TEXT NOT NULL)");
        for (const auto* service : {"fixture.context.bootstrap", "fixture.context.extra", "fixture.executor", "fixture.model.primary", "fixture.model.backup", "fixture.strategy"}) host.provide(service);
        const auto record = contracts::object(Json::object(), Json::array(), true);
        // Two independently typed actions share the same executor transport.
        host.action({{"id", "fixture.number"}, {"service", "fixture.executor"}, {"revision", 1}, {"visibility", "scoped"},
            {"description", "Fixture integer"}, {"input", contracts::object({{"value", {{"type", "integer"}}}}, Json::array({"value"}))}, {"output", record}, {"effects", "read"}, {"resource", "instance"}});
        host.action({{"id", "fixture.text"}, {"service", "fixture.executor"}, {"revision", 2}, {"visibility", "scoped"},
            {"description", "Fixture text"}, {"input", contracts::object({{"text", {{"type", "string"}}}}, Json::array({"text"}))}, {"output", record}, {"effects", "local-write"}, {"resource", "instance"}});
    }
    void start() { if (config_.value("kind", std::string("conversation")) == "instance") host.open_scope("lease", "instance"); }
    void on_event(const Json& event) {
        record(event);
        const auto type = event.at("type").get<std::string>();
        if (type == "scope_restored") {
            auto previous = db_.prepare("SELECT body FROM observations WHERE json_extract(body,'$.type')='scope_reply' AND json_extract(body,'$.key')='lease' ORDER BY seq DESC LIMIT 1");
            if (previous.step()) host.request_scoped(Json::parse(previous.column(0)).at("result").at("id"), "stale-token", "memory", "snapshot", Json::object());
            activate(event.at("result").at("id")); return;
        }
        if (type == "scope_closed") { if (event.at("scope_id") == scope_) scope_.clear(); return; }
        if (type == "facts") { if (config_.value("ack", true)) host.acknowledge(event.at("registration_id"), event.at("page").at("cursor")); return; }
        if (type == "scope_reply") {
            if (!event.at("error").is_null()) { collect_done(); return; }
            if (event.at("key") == "lease") activate(event.at("result").at("id"));
            else if (event.at("key") == "facts") collect_done();
            else if (event.at("key") == "context" || event.at("key") == "number" || event.at("key") == "text") registrations_.push_back(event.at("result").at("id"));
            else if (event.at("key") == "closed") collect_done();
            return;
        }
        if (type != "call") return;
        const Call call(event);
        if (call.service == "fixture.context.bootstrap") {
            const auto text = call.input.at("text").get<std::string>();
            if (text == "skip") { host.complete(call.id, {{"items", Json::array()}}); return; }
            pending_ = call.id;
            if (text == "release") {
                for (const auto& registration : registrations_) host.release("released", registration);
                registrations_.clear(); collect_done(); return;
            }
            if (text == "close" && scope_.empty()) { collect_done(); return; }
            if (text == "close" && !scope_.empty()) {
                const auto old = scope_; host.close_scope("closed", old); host.request_scoped(old, "after-close", "memory", "snapshot", Json::object()); return;
            }
            host.open_scope("lease", config_.value("kind", std::string("conversation")), call.id); return;
        }
        if (call.service == "fixture.context.extra") {
            host.complete(call.id, {{"items", Json::array({{{"source", "scope-fixture"}, {"revision", 1}, {"scope", call.input.at("scope")}, {"content", "scoped context"}}})}}); return;
        }
        if (call.service == "fixture.executor") {
            const ActionInvocation invocation(call);
            host.complete(call.id, outcome(invocation.arguments, invocation.action_id == "fixture.text" ? "confirmed" : "none")); return;
        }
        if (call.service == "fixture.strategy") {
            const auto view = RunView::from_json(call.input.at("run")); const auto state = call.input.at("state");
            if (call.method == "review_finish") { host.complete(call.id, {{"accept", true}, {"state", state}}); return; }
            Json proposal{{"type", "request_model"}};
            if (view.observation.value("kind", std::string{}) == "model_failed") proposal["binding"] = config_.value("fallback", std::string("model.backup"));
            else if (!view.candidate.is_null()) proposal = {{"type", "finish"}, {"text", view.candidate.at("text")}, {"goal", "met"}};
            host.complete(call.id, {{"state", state}, {"proposal", proposal}}); return;
        }
        if (call.service == "fixture.model.primary") {
            if (config_.value("failure", std::string{}) == "malformed") host.complete(call.id, {{"events", Json::array()}});
            else if (config_.value("failure", std::string{}) == "deadline") { std::this_thread::sleep_for(std::chrono::milliseconds(350)); host.complete(call.id, {{"events", Json::array()}}); }
            else host.fail(call.id, error("FIXTURE_PROVIDER_UNAVAILABLE", "unavailable"));
            return;
        }
        host.complete(call.id, {{"events", Json::array({{{"type", "final"}, {"text", "backup result"}, {"goal", "met"}}})}});
    }
};
DFY_PLUGIN(ScopeProbe)
