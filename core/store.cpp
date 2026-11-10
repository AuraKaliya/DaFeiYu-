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

    // 取得状态目录的进程级独占锁，阻止实例并发占用。
    explicit Impl(const Path& path) {
        handle = CreateFileW(
            path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        require(handle != INVALID_HANDLE_VALUE, "INSTANCE_LOCK_UNAVAILABLE");
    }

    // 释放实例独占锁及对应系统句柄。
    ~Impl() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
#else
    int handle{-1};

    // 取得状态目录的进程级独占锁，阻止实例并发占用。
    explicit Impl(const Path& path) {
        handle = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        require(handle >= 0, "INSTANCE_LOCK_UNAVAILABLE");

        if (flock(handle, LOCK_EX | LOCK_NB) != 0) {
            close(handle);
            handle = -1;
            throw std::runtime_error("INSTANCE_IN_USE");
        }
    }

    // 释放实例独占锁及对应系统句柄。
    ~Impl() {
        if (handle >= 0) {
            flock(handle, LOCK_UN);
            close(handle);
        }
    }
#endif
};

// 创建状态目录并取得实例锁文件的独占所有权。
InstanceLock::InstanceLock(const Path& directory) {
    std::filesystem::create_directories(directory);

    impl_ = std::make_unique<Impl>(directory / "instance.lock");
}

// 通过锁实现的析构释放实例独占锁。
InstanceLock::~InstanceLock() = default;

// 保存状态所有者身份与写令牌，限定后续写入权限。
StateWriter::StateWriter(StateStore& store, std::string token, std::string owner)
    : store_(store), token_(std::move(token)), owner_(std::move(owner)) {
}

// 以当前所有者令牌写入指定状态对象。
void StateWriter::put(std::string_view kind, std::string_view id, const Json& value) {
    store_.put(token_, kind, id, value);
}

// 以当前所有者令牌删除指定状态对象。
void StateWriter::erase(std::string_view kind, std::string_view id) {
    store_.erase(token_, kind, id);
}

// 以当前所有者身份追加事实事件。
void StateWriter::event(std::string type, const Json& payload, std::string run, std::string source, std::string cause,
    std::string conversation) {
    store_.event(
        owner_, std::move(type), payload, std::move(run), std::move(source), std::move(cause), std::move(conversation));
}

// 在同一事务内保存状态对象并追加对应事实事件。
void StateWriter::save(std::string_view kind, std::string_view id, const Json& value, std::string type,
    const Json& payload, std::string run, std::string conversation) {
    store_.atomic([&] {
        put(kind, id, value);
        event(std::move(type), payload, std::move(run), "core", {}, std::move(conversation));
    });
}

// 打开核心状态库，并建立实例身份、状态索引和不可变事实日志。
// 步骤：取得实例锁并打开数据库 → 恢复或创建实例身份 → 校验存储版本 → 建立对象、资源和事实表及约束。
StateStore::StateStore(const Path& directory) : lock_(directory), db_(directory / "core.sqlite3") {
    db_.exec(
        "CREATE TABLE IF NOT EXISTS identity(id INTEGER PRIMARY KEY CHECK(id=1),instance TEXT NOT NULL,component TEXT NOT NULL,version INTEGER NOT NULL)");

    {
        auto query = db_.prepare("SELECT instance FROM identity WHERE id=1");
        instance_ = query.step() ? query.column(0) : identifier();
    }
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

// 为状态所有者分配互不重叠的写入范围及令牌。
// 步骤：校验核心线程 → 检查对象类型或指定对象的范围冲突 → 登记范围并返回写入器。
StateWriter StateStore::claim(std::string owner, std::initializer_list<std::string> selectors) {
    require(thread_ == std::this_thread::get_id(), "STATE_THREAD_DENIED");

    const auto token = identifier();
    for (auto selector : selectors) {
        if (selector.find('/') == std::string::npos)
            selector += "/*";
        const auto kind = selector.substr(0, selector.find('/'));
        for (const auto& [existing, value] : owners_) {
            (void)value;
            require(existing != selector && existing != kind + "/*" &&
                        !(selector == kind + "/*" && existing.starts_with(kind + "/")),
                "STATE_OWNER_ALREADY_CLAIMED");
        }
    }

    for (auto selector : selectors) {
        if (selector.find('/') == std::string::npos)
            selector += "/*";
        owners_.emplace(selector, token);
    }
    return StateWriter(*this, token, std::move(owner));
}

// 校验写入发生在所属线程、授权对象范围及活动事务内。
void StateStore::check_owner(std::string_view token, std::string_view kind, std::string_view id) {
    require(thread_ == std::this_thread::get_id(), "STATE_THREAD_DENIED");
    const auto all = owners_.find(std::string(kind) + "/*"),
               exact = owners_.find(std::string(kind) + "/" + std::string(id));
    require((all != owners_.end() && all->second == token) || (exact != owners_.end() && exact->second == token),
        "STATE_OWNER_DENIED");
    require(transaction_depth_ != 0, "STATE_WRITE_OUTSIDE_TRANSACTION");
}

// 以事务或嵌套保存点执行状态变更，并仅发布已提交的变化。
// 步骤：校验线程并保存暂存变更 → 按嵌套层次开启事务或保存点 → 成功时提交 → 异常时回滚并恢复暂存集合。
void StateStore::atomic(const std::function<void()>& action) {
    require(thread_ == std::this_thread::get_id(), "STATE_THREAD_DENIED");

    const auto before = staged_;
    if (transaction_depth_) {
        const auto name = "tx_" + std::to_string(++transaction_serial_);
        db_.exec("SAVEPOINT " + name);
        ++transaction_depth_;
        try {
            action();
            db_.exec("RELEASE " + name);
            --transaction_depth_;
        } catch (...) {
            db_.exec("ROLLBACK TO " + name);
            db_.exec("RELEASE " + name);
            staged_ = before;
            --transaction_depth_;
            throw;
        }
        return;
    }

    Transaction transaction(db_);
    ++transaction_depth_;
    try {
        action();
        transaction.commit();
        --transaction_depth_;
        committed_.insert(staged_.begin(), staged_.end());
        staged_.clear();
    } catch (...) {
        staged_ = before;
        --transaction_depth_;
        throw;
    }
}

// 取出已提交对象的变化通知，并清空已消费集合。
std::vector<StateChange> StateStore::take_changes() {
    std::vector<StateChange> result;
    for (const auto& [kind, id] : committed_)
        result.push_back({kind, id});
    committed_.clear();
    return result;
}

// 返回队列查询行数、扫描和虚拟机执行计数。
Json StateStore::metrics() const {
    return {{"queue_queries", queue_queries_}, {"queue_rows", queue_rows_}, {"queue_fullscan_steps", fullscan_steps_},
        {"queue_vm_steps", vm_steps_}};
}

// 按类型和标识读取单个状态对象，缺失时返回空值。
Json StateStore::get(std::string_view kind, std::string_view id) {
    auto query = db_.prepare("SELECT body FROM objects WHERE kind=? AND id=?");
    query.bind(1, kind).bind(2, id);
    return query.step() ? Json::parse(query.column(0)) : Json{};
}

// 执行带参数的状态查询，并累计查询开销指标。
// 步骤：绑定参数 → 解析结果对象 → 累计返回行数和执行统计。
Json StateStore::query(std::string_view sql, const std::vector<std::string>& parameters) {
    auto statement = db_.prepare(sql);
    for (size_t i = 0; i < parameters.size(); ++i)
        statement.bind(static_cast<int>(i + 1), parameters[i]);

    Json result = Json::array();
    ++queue_queries_;
    while (statement.step()) {
        result.push_back(Json::parse(statement.column(0)));
        ++queue_rows_;
    }

    fullscan_steps_ += statement.fullscan_steps();
    vm_steps_ += statement.vm_steps();
    return result;
}

// 读取允许全量扫描的状态类型，拒绝扫描大体量任务历史。
Json StateStore::all(std::string_view kind) {
    require(kind != "run" && kind != "input" && kind != "operation" && kind != "job" && kind != "occurrence",
        "HISTORY_SCAN_FORBIDDEN");
    return query("SELECT body FROM objects WHERE kind=? ORDER BY seq", {std::string(kind)});
}

// 读取尚未关闭的作用域或注册等存活记录。
Json StateStore::live(std::string_view kind) {
    return query(
        "SELECT body FROM objects INDEXED BY objects_kind_state_seq WHERE kind=? AND state IN('active','suspended') ORDER BY seq",
        {std::string(kind)});
}

// 按对象序号分页读取指定类型的历史状态。
// 步骤：校验页大小 → 查询游标后的记录 → 构造条目、后续游标和更多标记。
Json StateStore::page(std::string_view kind, uint64_t after, size_t limit) {
    require(limit > 0 && limit <= 200, "PAGE_LIMIT");

    auto statement = db_.prepare("SELECT seq,body FROM objects WHERE kind=? AND seq>? ORDER BY seq LIMIT ?");
    statement.bind(1, kind).bind(2, std::to_string(after)).bind(3, std::to_string(limit + 1));
    Json items = Json::array();
    uint64_t cursor = after;
    bool more = false;

    while (statement.step()) {
        if (items.size() == limit) {
            more = true;
            break;
        }
        cursor = std::stoull(statement.column(0));
        items.push_back(Json::parse(statement.column(1)));
    }
    return {{"items", items}, {"cursor", cursor}, {"has_more", more}};
}

// 按入库顺序读取尚未消费的输入。
Json StateStore::pending_inputs(size_t limit) {
    return query("SELECT body FROM objects WHERE kind='input' AND state='accepted' ORDER BY seq LIMIT ?",
        {std::to_string(limit)});
}

// 统计当前等待消费的输入数量。
size_t StateStore::pending_input_count() {
    auto statement = db_.prepare("SELECT count(*) FROM objects WHERE kind='input' AND state='accepted'");
    statement.step();
    return std::stoull(statement.column(0));
}

// 读取尚未终止的运行任务。
Json StateStore::active_runs() {
    return query(
        "SELECT body FROM objects INDEXED BY objects_kind_state_seq WHERE kind='run' AND state IN('queued','running','waiting','paused') ORDER BY seq");
}

// 按调度类别读取已排队或定时等待已到期的任务。
// 步骤：构造操作、用户或活动过滤条件 → 合并排队与到期等待任务 → 按序号限量返回。
Json StateStore::ready_runs(int64_t now, size_t limit, int category, const Json& activities) {
    std::string filter;

    std::vector<std::string> filter_args;
    if (category == 1)
        return query(
            "SELECT body FROM objects WHERE kind='run' AND state='queued' AND phase IN('action','delivery') ORDER BY seq LIMIT ?",
            {std::to_string(limit)});
    if (category == 2)
        filter = " AND activity_id=''";

    if (category == 3) {
        if (activities.empty())
            return Json::array();
        filter = " AND activity_id IN(";
        for (const auto& id : activities) {
            if (!filter_args.empty())
                filter += ',';
            filter += '?';
            filter_args.push_back(id.get<std::string>());
        }
        filter += ')';
    }

    const auto phases = category ? " AND phase NOT IN('action','delivery')" : "";
    std::vector<std::string> args = filter_args;
    args.push_back(std::to_string(now));
    args.insert(args.end(), filter_args.begin(), filter_args.end());
    args.push_back(std::to_string(limit));
    return query(
        "SELECT body FROM (SELECT seq,body FROM objects WHERE kind='run' AND state='queued'" + filter + phases +
            " UNION ALL "
            "SELECT seq,body FROM objects WHERE kind='run' AND state='waiting' AND phase='wait' AND due_at>0 AND due_at<=?" +
            filter + ") ORDER BY seq LIMIT ?",
        args);
}

// 读取指定会话中尚未终止的运行任务。
Json StateStore::runs_in_conversation(const std::string& id) {
    return query(
        "SELECT body FROM objects WHERE kind='run' AND conversation_id=? AND state IN('queued','running','waiting','paused')",
        {id});
}

// 读取关联指定操作且仍可接收结算通知的运行任务。
Json StateStore::runs_waiting_for_operation(const std::string& id) {
    return query(
        "SELECT body FROM objects WHERE kind='run' AND operation_id=? AND state IN('queued','running','waiting')",
        {id});
}

// 查找任务尚未消费的新操作结算版本，补偿遗漏通知。
Json StateStore::unobserved_operations() {
    return query(
        "SELECT o.body FROM objects r JOIN objects o ON o.kind='operation' AND o.id=r.operation_id WHERE r.kind='run' AND r.state IN('waiting','queued') AND o.state IN('settled','unknown') AND json_extract(r.body,'$.observed_operation_revision')<json_extract(o.body,'$.revision') LIMIT 64");
}

// 读取已终止但尚未反馈给活动提供者的运行任务。
Json StateStore::activity_feedback() {
    return query("SELECT body FROM objects WHERE kind='run' AND feedback=1 ORDER BY seq LIMIT 64");
}

// 读取运行任务的操作记录，可限定为未结算操作。
Json StateStore::run_operations(const std::string& id, bool unresolved) {
    return query(unresolved
                     ? "SELECT body FROM objects WHERE kind='operation' AND run_id=? AND state<>'settled' ORDER BY seq"
                     : "SELECT body FROM objects WHERE kind='operation' AND run_id=? ORDER BY seq",
        {id});
}

// 读取指定父操作的全部直接子操作。
Json StateStore::child_operations(const std::string& id) {
    return query("SELECT body FROM objects WHERE kind='operation' AND parent_id=? ORDER BY seq", {id});
}

// 读取重启后需要恢复的运行中或已交接作业的操作。
Json StateStore::recoverable_operations() {
    return query(
        "SELECT body FROM objects INDEXED BY objects_kind_state_seq WHERE kind='operation' AND state IN('running','job') ORDER BY seq");
}

// 查找与指定资源集合相交的未知副作用操作。
Json StateStore::unknown_operations(const Json& resources) {
    std::string sql =
        "SELECT o.body FROM objects o INDEXED BY objects_kind_state_seq WHERE o.kind='operation' AND o.state='unknown' AND EXISTS (SELECT 1 FROM object_resources r WHERE r.kind=o.kind AND r.id=o.id AND r.resource IN(";

    std::vector<std::string> args;
    for (const auto& resource : resources) {
        if (!args.empty())
            sql += ",";
        sql += "?";
        args.push_back(resource.get<std::string>());
    }
    if (args.empty())
        return Json::array();

    return query(sql + "))", args);
}

// 读取实例或指定运行任务中尚需跟踪的后台作业。
Json StateStore::active_jobs(const std::string& run) {
    return run.empty()
               ? query(
                     "SELECT body FROM objects INDEXED BY objects_kind_state_seq WHERE kind='job' AND state IN('registered','running','interrupted') ORDER BY seq")
               : query("SELECT body FROM objects WHERE kind='job' AND run_id=? AND state<>'completed' ORDER BY seq",
                     {run});
}

// 读取已到重试期限的待评估活动触发记录。
Json StateStore::pending_occurrences(const std::string& activity) {
    return activity.empty()
               ? query(
                     "SELECT body FROM objects WHERE kind='occurrence' AND state='pending' AND due_at<=? ORDER BY seq LIMIT 64",
                     {std::to_string(now_ms())})
               : query(
                     "SELECT body FROM objects WHERE kind='occurrence' AND activity_id=? AND state='pending' AND due_at<=? ORDER BY seq LIMIT 64",
                     {activity, std::to_string(now_ms())});
}

// 判断活动是否仍有已接收输入或未终止运行任务。
bool StateStore::active_activity_work(const std::string& activity) {
    auto statement = db_.prepare(
        "SELECT 1 FROM objects WHERE kind='input' AND activity_id=? AND state='accepted' UNION ALL "

        "SELECT 1 FROM objects WHERE kind='run' AND activity_id=? AND state IN('queued','running','waiting','paused') LIMIT 1");
    statement.bind(1, activity).bind(2, activity);
    return statement.step();
}

// 判断指定活动是否已有待评估触发记录。
bool StateStore::pending_activity_occurrence(const std::string& activity) {
    auto statement =
        db_.prepare("SELECT 1 FROM objects WHERE kind='occurrence' AND activity_id=? AND state='pending' LIMIT 1");
    statement.bind(1, activity);
    return statement.step();
}

// 读取当前启用且已到触发期限的活动。
Json StateStore::due_activities(int64_t now) {
    return query(
        "SELECT a.body FROM objects a JOIN objects p ON p.kind='activity_policy' AND p.id=a.id WHERE a.kind='activity' AND a.state='registered' AND a.due_at<=? AND json_extract(p.body,'$.enabled')=1 ORDER BY a.due_at LIMIT 64",
        {std::to_string(now)});
}

// 计算活动触发、任务定时等待和触发重试中的最早期限。
int64_t StateStore::next_wakeup() {
    auto statement = db_.prepare(
        "SELECT MIN(value) FROM (SELECT MIN(a.due_at) AS value FROM objects a JOIN objects p ON p.kind='activity_policy' AND p.id=a.id WHERE a.kind='activity' AND a.state='registered' AND json_extract(p.body,'$.enabled')=1 UNION ALL "
        "SELECT MIN(due_at) FROM objects WHERE kind='run' AND state='waiting' AND phase='wait' AND due_at>0 UNION ALL SELECT MIN(due_at) FROM objects WHERE kind='occurrence' AND state='pending' AND due_at>0)");
    statement.step();
    const auto value = statement.column(0);
    return value.empty() ? 0 : std::stoll(value);
}

// 校验写权限后保存对象，并暂存提交后的变化通知。
void StateStore::put(std::string_view token, std::string_view kind, std::string_view id, const Json& value) {
    check_owner(token, kind, id);

    if (value.contains("id"))
        require(value.at("id").get<std::string>() == id, "STATE_ID_MISMATCH");

    auto write = db_.prepare(
        "INSERT INTO objects(kind,id,body) VALUES(?,?,?) ON CONFLICT(kind,id) DO UPDATE SET body=excluded.body");
    write.bind(1, kind).bind(2, id).bind(3, value.dump()).step();

    staged_.emplace(std::string(kind), std::string(id));
}

// 校验写权限后删除对象，并暂存提交后的变化通知。
void StateStore::erase(std::string_view token, std::string_view kind, std::string_view id) {
    check_owner(token, kind, id);

    auto write = db_.prepare("DELETE FROM objects WHERE kind=? AND id=?");
    write.bind(1, kind).bind(2, id).step();
    staged_.emplace(std::string(kind), std::string(id));
}

// 在所有者事务内追加带实例、任务和会话身份的事实事件。
// 步骤：校验事务与线程 → 补齐会话身份 → 构造事实记录并写入不可变日志。
void StateStore::event(std::string owner, std::string type, const Json& payload, std::string run, std::string source,
    std::string cause, std::string conversation) {
    require(transaction_depth_ != 0 && thread_ == std::this_thread::get_id(), "EVENT_OUTSIDE_OWNER_TRANSACTION");

    if (conversation.empty() && !run.empty()) {
        const auto record = get("run", run);
        if (!record.is_null())
            conversation = record.value("conversation", std::string{});
    }

    const auto id = identifier();
    Json value{{"id", id}, {"instance_id", instance_id()}, {"type", type}, {"run_id", run},
        {"conversation_id", conversation}, {"source", source}, {"owner", owner}, {"cause", cause},
        {"received_at", now_ms()}, {"version", 2}, {"payload", payload}};

    auto insert = db_.prepare("INSERT INTO facts(id,type,run,conversation,body) VALUES(?,?,?,?,?)");
    insert.bind(1, id).bind(2, type).bind(3, run).bind(4, conversation).bind(5, value.dump()).step();
}

// 按游标及任务、类型或会话条件分页读取事实事件。
// 步骤：校验页大小并构造过滤条件 → 读取事件并附加序号 → 计算游标和更多标记。
Json StateStore::events(
    uint64_t after, size_t limit, const std::string& run, const Json& types, const std::string& conversation) {
    require(limit > 0 && limit <= 200, "EVENT_PAGE_LIMIT");

    std::string sql = "SELECT seq,body FROM facts WHERE seq>?";
    std::vector<std::string> args{std::to_string(after)};

    if (!run.empty()) {
        sql += " AND run=?";
        args.push_back(run);
    }
    if (!conversation.empty()) {
        sql += " AND conversation=?";
        args.push_back(conversation);
    }
    if (!types.empty()) {
        sql += " AND type IN(";
        for (const auto& type : types) {
            if (sql.back() != '(')
                sql += ',';
            sql += '?';
            args.push_back(type.get<std::string>());
        }
        sql += ')';
    }
    sql += " ORDER BY seq LIMIT ?";
    args.push_back(std::to_string(limit + 1));

    auto statement = db_.prepare(sql);
    for (size_t i = 0; i < args.size(); ++i)
        statement.bind(static_cast<int>(i + 1), args[i]);

    Json values = Json::array();
    uint64_t cursor = after;
    bool more = false;
    while (statement.step()) {
        if (values.size() == limit) {
            more = true;
            break;
        }
        cursor = std::stoull(statement.column(0));
        auto value = Json::parse(statement.column(1));
        value["sequence"] = cursor;
        values.push_back(std::move(value));
    }

    if (!more)
        cursor = std::max(cursor, sequence());
    return {{"events", values}, {"cursor", cursor}, {"has_more", more}};
}

// 取得当前事实日志的最大序号。
uint64_t StateStore::sequence() {
    auto query = db_.prepare("SELECT COALESCE(MAX(seq),0) FROM facts");
    query.step();
    return std::stoull(query.column(0));
}
}
