#pragma once
#include "common.hpp"
#include <dafeiyu/sqlite.hpp>
#include <thread>

namespace dfy {
class InstanceLock {
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    // 创建状态目录并取得实例锁文件的独占所有权。
    explicit InstanceLock(const Path& directory);
    // 通过锁实现的析构释放实例独占锁。
    ~InstanceLock();
};
class StateStore;

class StateWriter {
    StateStore& store_;
    std::string token_, owner_;
    // 保存状态所有者身份与写令牌，限定后续写入权限。
    StateWriter(StateStore& store, std::string token, std::string owner);
    friend class StateStore;

public:
    // 禁止复制或赋值，保持当前对象独占的资源与状态所有权。
    StateWriter(const StateWriter&) = delete;
    // 禁止复制或赋值，保持当前对象独占的资源与状态所有权。
    StateWriter& operator=(const StateWriter&) = delete;
    // 以当前所有者令牌写入指定状态对象。
    void put(std::string_view kind, std::string_view id, const Json& value);
    // 以当前所有者令牌删除指定状态对象。
    void erase(std::string_view kind, std::string_view id);
    // 以当前所有者身份追加事实事件。
    void event(std::string type, const Json& payload, std::string run = {}, std::string source = "core",
        std::string cause = {}, std::string conversation = {});
    // 在同一事务内保存状态对象并追加对应事实事件。
    void save(std::string_view kind, std::string_view id, const Json& value, std::string type,
        const Json& payload = Json::object(), std::string run = {}, std::string conversation = {});
};

struct StateChange {
    std::string kind, id;
};

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
    // 校验写入发生在所属线程、授权对象范围及活动事务内。
    void check_owner(std::string_view token, std::string_view kind, std::string_view id);
    // 校验写权限后保存对象，并暂存提交后的变化通知。
    void put(std::string_view token, std::string_view kind, std::string_view id, const Json& value);
    // 校验写权限后删除对象，并暂存提交后的变化通知。
    void erase(std::string_view token, std::string_view kind, std::string_view id);
    // 在所有者事务内追加带实例、任务和会话身份的事实事件。
    void event(std::string owner, std::string type, const Json& payload, std::string run, std::string source,
        std::string cause, std::string conversation);
    // 执行带参数的状态查询，并累计查询开销指标。
    Json query(std::string_view sql, const std::vector<std::string>& parameters = {});
    friend class StateWriter;

public:
    // 打开核心状态库，并建立实例身份、状态索引和不可变事实日志。
    explicit StateStore(const Path& directory);

    // 返回当前状态库所属的稳定实例标识。
    const std::string& instance_id() const { return instance_; }

    // 为状态所有者分配互不重叠的写入范围及令牌。
    StateWriter claim(std::string owner, std::initializer_list<std::string> selectors);
    // 以事务或嵌套保存点执行状态变更，并仅发布已提交的变化。
    void atomic(const std::function<void()>& action);

    // 判断是否存在尚未消费的已提交状态变化。
    bool changed() const { return !committed_.empty(); }

    // 取出已提交对象的变化通知，并清空已消费集合。
    std::vector<StateChange> take_changes();
    // 返回队列查询行数、扫描和虚拟机执行计数。
    Json metrics() const;
    // 按类型和标识读取单个状态对象，缺失时返回空值。
    Json get(std::string_view kind, std::string_view id);
    // 读取允许全量扫描的状态类型，拒绝扫描大体量任务历史。
    Json all(std::string_view kind);
    // 读取尚未关闭的作用域或注册等存活记录。
    Json live(std::string_view kind);
    // 按对象序号分页读取指定类型的历史状态。
    Json page(std::string_view kind, uint64_t after = 0, size_t limit = 100);
    // 按入库顺序读取尚未消费的输入。
    Json pending_inputs(size_t limit = 64);
    // 统计当前等待消费的输入数量。
    size_t pending_input_count();
    // 读取尚未终止的运行任务。
    Json active_runs();
    // 按调度类别读取已排队或定时等待已到期的任务。
    Json ready_runs(int64_t now, size_t limit = 64, int category = 0, const Json& activities = Json::array());
    // 读取指定会话中尚未终止的运行任务。
    Json runs_in_conversation(const std::string& conversation);
    // 读取关联指定操作且仍可接收结算通知的运行任务。
    Json runs_waiting_for_operation(const std::string& operation);
    // 查找任务尚未消费的新操作结算版本，补偿遗漏通知。
    Json unobserved_operations();
    // 读取已终止但尚未反馈给活动提供者的运行任务。
    Json activity_feedback();
    // 读取运行任务的操作记录，可限定为未结算操作。
    Json run_operations(const std::string& run, bool unresolved_only = false);
    // 读取指定父操作的全部直接子操作。
    Json child_operations(const std::string& parent);
    // 读取重启后需要恢复的运行中或已交接作业的操作。
    Json recoverable_operations();
    // 查找与指定资源集合相交的未知副作用操作。
    Json unknown_operations(const Json& resources);
    // 读取实例或指定运行任务中尚需跟踪的后台作业。
    Json active_jobs(const std::string& run = {});
    // 读取已到重试期限的待评估活动触发记录。
    Json pending_occurrences(const std::string& activity = {});
    // 判断指定活动是否已有待评估触发记录。
    bool pending_activity_occurrence(const std::string& activity);
    // 判断活动是否仍有已接收输入或未终止运行任务。
    bool active_activity_work(const std::string& activity);
    // 读取当前启用且已到触发期限的活动。
    Json due_activities(int64_t now);
    // 计算活动触发、任务定时等待和触发重试中的最早期限。
    int64_t next_wakeup();
    // 按游标及任务、类型或会话条件分页读取事实事件。
    Json events(uint64_t after = 0, size_t limit = 100, const std::string& run = {}, const Json& types = Json::array(),
        const std::string& conversation = {});
    // 取得当前事实日志的最大序号。
    uint64_t sequence();
};
}
