#pragma once
#include "common.hpp"
#include <dafeiyu/sqlite.hpp>
#include <thread>

namespace dfy {
class InstanceLock {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit InstanceLock(const Path& directory);
    ~InstanceLock();
};
class StateStore;
class StateWriter {
    StateStore& store_;
    std::string token_, owner_;
    StateWriter(StateStore& store, std::string token, std::string owner);
    friend class StateStore;
public:
    StateWriter(const StateWriter&) = delete;
    StateWriter& operator=(const StateWriter&) = delete;
    void put(std::string_view kind, std::string_view id, const Json& value);
    void erase(std::string_view kind, std::string_view id);
    void event(std::string type, const Json& payload, std::string run = {}, std::string source = "core", std::string cause = {}, std::string conversation = {});
    void save(std::string_view kind, std::string_view id, const Json& value, std::string type, const Json& payload = Json::object(), std::string run = {}, std::string conversation = {});
};
struct StateChange { std::string kind, id; };
class StateStore {
    InstanceLock lock_;
    Database db_;
    std::string instance_;
    unsigned transaction_depth_{};
    uint64_t transaction_serial_{};
    std::map<std::string, std::string> owners_;
    std::set<std::pair<std::string, std::string>> staged_, committed_;
    uint64_t queue_queries_{}, queue_rows_{}, fullscan_steps_{}, vm_steps_{};
    std::thread::id thread_ = std::this_thread::get_id();
    void check_owner(std::string_view token, std::string_view kind, std::string_view id);
    void put(std::string_view token, std::string_view kind, std::string_view id, const Json& value);
    void erase(std::string_view token, std::string_view kind, std::string_view id);
    void event(std::string owner, std::string type, const Json& payload, std::string run, std::string source, std::string cause, std::string conversation);
    Json query(std::string_view sql, const std::vector<std::string>& parameters = {});
    friend class StateWriter;
public:
    explicit StateStore(const Path& directory);
    const std::string& instance_id() const { return instance_; }
    StateWriter claim(std::string owner, std::initializer_list<std::string> selectors);
    void atomic(const std::function<void()>& action);
    bool changed() const { return !committed_.empty(); }
    std::vector<StateChange> take_changes();
    Json metrics() const;
    Json get(std::string_view kind, std::string_view id);
    Json all(std::string_view kind);
    Json live(std::string_view kind);
    Json page(std::string_view kind, uint64_t after = 0, size_t limit = 100);
    Json pending_inputs(size_t limit = 64);
    size_t pending_input_count();
    Json active_runs();
    Json ready_runs(int64_t now, size_t limit = 64, int category = 0, const Json& activities = Json::array());
    Json runs_in_conversation(const std::string& conversation);
    Json runs_waiting_for_operation(const std::string& operation);
    Json unobserved_operations();
    Json activity_feedback();
    Json run_operations(const std::string& run, bool unresolved_only = false);
    Json child_operations(const std::string& parent);
    Json recoverable_operations();
    Json unknown_operations(const Json& resources);
    Json active_jobs(const std::string& run = {});
    Json pending_occurrences(const std::string& activity = {});
    bool pending_activity_occurrence(const std::string& activity);
    bool active_activity_work(const std::string& activity);
    Json due_activities(int64_t now);
    int64_t next_wakeup();
    Json events(uint64_t after = 0, size_t limit = 100, const std::string& run = {}, const Json& types = Json::array(), const std::string& conversation = {});
    uint64_t sequence();
};
}
