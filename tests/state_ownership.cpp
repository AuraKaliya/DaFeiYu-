#include "store.hpp"
#include <iostream>
#include <thread>
using namespace dfy;
namespace {
void check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
template<class Fn> void rejected(Fn fn, const std::string& code) {
    try { fn(); } catch (const std::exception& error) { check(std::string(error.what()).find(code) != std::string::npos, error.what()); return; }
    throw std::runtime_error("expected " + code);
}
}
int main(int argc, char** argv) {
    try {
        check(argc == 2, "state test WORK");
        StateStore store(sdk::path_from_utf8(argv[1]) / ("ownership-" + identifier()));
        auto runs = store.claim("runs", {"run"}), operations = store.claim("operations", {"operation"});
        auto inbox = store.claim("inbox", {"input"}), jobs = store.claim("jobs", {"job"});
        auto behavior = store.claim("behavior", {"system/behavior"});
        rejected([&] { auto duplicate = store.claim("intruder", {"run"}); (void)duplicate; }, "STATE_OWNER_ALREADY_CLAIMED");
        rejected([&] { auto duplicate = store.claim("intruder", {"system"}); (void)duplicate; }, "STATE_OWNER_ALREADY_CLAIMED");
        rejected([&] { runs.put("run", "outside", Json::object()); }, "STATE_WRITE_OUTSIDE_TRANSACTION");
        rejected([&] { store.atomic([&] { inbox.put("run", "wrong", Json::object()); }); }, "STATE_OWNER_DENIED");
        rejected([&] { store.atomic([&] { behavior.put("system", "policy", Json::object()); }); }, "STATE_OWNER_DENIED");
        std::thread outsider([&] { rejected([&] { runs.save("run", "thread", Json::object(), "bad"); }, "STATE_THREAD_DENIED"); }); outsider.join();
        const auto sequence = store.sequence();
        rejected([&] { store.atomic([&] { runs.save("run", "rolled", {{"id", "rolled"}}, "rolled"); throw std::runtime_error("rollback"); }); }, "rollback");
        check(store.get("run", "rolled").is_null() && !store.changed() && store.sequence() == sequence, "rollback published state, facts or notifications");
        store.atomic([&] {
            runs.put("run", "kept", {{"id", "kept"}, {"execution", "completed"}});
            rejected([&] { store.atomic([&] { operations.save("operation", "nested", {{"id", "nested"}}, "nested"); throw std::runtime_error("nested-rollback"); }); }, "nested-rollback");
            check(!store.changed(), "uncommitted notification escaped");
        });
        const auto notes = store.take_changes();
        check(notes.size() == 1 && notes[0].id == "kept" && store.get("operation", "nested").is_null() && store.sequence() == sequence, "nested savepoint broke outer commit");
        // Large completed histories must not turn a scheduler query into a history walk.
        store.atomic([&] {
            for (int i = 0; i < 10000; ++i) {
                const auto id = std::to_string(i);
                runs.put("run", id, {{"id", id}, {"execution", "completed"}, {"activity", ""}});
                inbox.put("input", id, {{"id", id}, {"state", "consumed"}});
                operations.put("operation", id, {{"id", id}, {"state", "settled"}, {"descriptor", {{"resource", "instance"}}}});
                jobs.put("job", id, {{"id", id}, {"state", "completed"}});
            }
            runs.put("run", "ready", {{"id", "ready"}, {"execution", "queued"}, {"phase", "decision"}, {"activity", ""}});
            runs.put("run", "waiting", {{"id", "waiting"}, {"execution", "waiting"}, {"phase", "action"}, {"active_operation", "effect"}, {"observed_operation_revision", 0}});
            operations.put("operation", "effect", {{"id", "effect"}, {"state", "settled"}, {"revision", 3}});
            operations.put("operation", "unknown", {{"id", "unknown"}, {"state", "unknown"}, {"descriptor", {{"resource", "instance"}}}});
            inbox.put("input", "pending", {{"id", "pending"}, {"state", "accepted"}});
            jobs.put("job", "active", {{"id", "active"}, {"state", "registered"}});
        });
        (void)store.take_changes(); // Simulate a missed notification; persisted work remains discoverable.
        const auto before = store.metrics();
        check(store.ready_runs(now_ms(), 64, 2).size() == 1 && store.pending_inputs().size() == 1 && store.pending_input_count() == 1, "indexed admission queue");
        check(store.active_runs().size() == 2 && store.active_jobs().size() == 1, "indexed live ownership");
        check(store.unobserved_operations().size() == 1, "missed settlement cannot be reconciled");
        check(store.unknown_operations(Json::array({"instance"})).size() == 1, "resource guard lost unknown effect");
        check(store.activity_feedback().empty() && store.due_activities(now_ms()).empty(), "historical rows became runnable");
        const auto after = store.metrics();
        check(after.at("queue_rows").get<int>() - before.at("queue_rows").get<int>() < 20, "scheduler decoded completed history");
        check(after.at("queue_vm_steps").get<int>() - before.at("queue_vm_steps").get<int>() < 2500, "scheduler walked historical index entries: " + after.dump());
        rejected([&] { (void)store.all("run"); }, "HISTORY_SCAN_FORBIDDEN");
        const auto page = store.page("run", 0, 2); check(page.at("items").size() == 2 && page.at("has_more") == true, "history pagination");
        check(store.page("run", page.at("cursor"), 2).at("items").at(0).at("id") != page.at("items").at(0).at("id"), "history cursor repeated rows");
        std::cout << "State ownership, rollback, recovery indexes and pagination passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
