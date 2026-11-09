#pragma once
#include "common.hpp"
#include <dafeiyu/sqlite.hpp>

namespace dfy {
class InstanceLock {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit InstanceLock(const Path& directory);
    ~InstanceLock();
};
class StateStore {
    InstanceLock lock_;
    Database db_;
    std::string instance_;
    unsigned transaction_depth_{};
public:
    explicit StateStore(const Path& directory);
    const std::string& instance_id() const { return instance_; }
    void atomic(const std::function<void()>& action);
    Json get(std::string_view kind, std::string_view id);
    Json all(std::string_view kind);
    void put(std::string_view kind, std::string_view id, const Json& value);
    void erase(std::string_view kind, std::string_view id);
    void event(std::string type, const Json& payload, std::string run = {}, std::string source = "core", std::string cause = {});
    void save(std::string_view kind, std::string_view id, const Json& value, std::string type, const Json& payload = Json::object(), std::string run = {});
    Json events(uint64_t after = 0, size_t limit = 100, const std::string& run = {}, const Json& types = Json::array());
    uint64_t sequence();
};
}
