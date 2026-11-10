#include <dafeiyu/plugin.hpp>
#include <dafeiyu/sqlite.hpp>
#include <atomic>
#include <map>
#include <thread>
using namespace dfy::sdk;

namespace {
// 取得测试后台作业使用的毫秒时间戳。
int64_t clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 从插件独立数据库读取指定后台工作记录。
Json load(dfy::Database& db, const std::string& id) {
    auto query = db.prepare("SELECT body FROM work WHERE id=?");
    query.bind(1, id);
    return query.step() ? Json::parse(query.column(0)) : Json{};
}

// 按工作标识插入或更新插件持有的后台工作状态。
void save(dfy::Database& db, const Json& value) {
    auto insert = db.prepare("INSERT INTO work VALUES(?,?) ON CONFLICT(id) DO UPDATE SET body=excluded.body");
    insert.bind(1, value.at("id").get<std::string>()).bind(2, value.dump()).step();
}
}

class Background : public Plugin {
    std::filesystem::path path_;
    dfy::Database db_;
    int delay_;
    std::map<uint64_t, Json> pending_;

    struct Observer {
        std::shared_ptr<std::atomic<bool>> done;
        std::jthread thread;
    };

    std::map<std::string, Observer> observers_;

    // 启动可停止的观察线程，推进并回传指定后台作业结果。
    // 步骤：去重观察请求 → 建立独立数据库观察线程 → 到期时保存结果并通知核心 → 标记观察结束。
    void observe(const std::string& job_id) {
        if (observers_.contains(job_id))
            return;
        auto done = std::make_shared<std::atomic<bool>>(false);

        auto thread = std::jthread(
            // 观察持久化后台工作，到期后保存并回传结果。
            // 步骤：建立独立数据库连接 → 循环检查停止信号与期限 → 事务完成工作并上报结果 → 标记观察线程退出。
            [this, job_id, done](std::stop_token stop) {
                try {
                    dfy::Database db(path_);

                    for (;;) {
                        if (stop.stop_requested())
                            break;

                        Json work;
                        {
                            dfy::Transaction transaction(db);
                            work = load(db, job_id);
                            if (work.at("state") != "completed" && clock_ms() >= work.at("due").get<int64_t>()) {
                                work["state"] = "completed";
                                work["result"] = outcome({{"value", work.at("value").get<int>() * 2}});
                                save(db, work);
                            }
                            transaction.commit();
                        }

                        if (work.at("state") == "completed") {
                            host.job_result(job_id, 1, work.at("result"));
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                } catch (const std::exception&) { // 观察失败后，核心仍保留可查询的作业登记。
                }

                done->store(true);
            });

        observers_.emplace(job_id, Observer{done, std::move(thread)});
    }

public:
    static constexpr auto id = "dafeiyu.action.background";
    static constexpr auto version = "0.5.0";

    // 初始化后台工作数据库、执行延迟及动作服务。
    Background(Host host, const Json& config)
        : Plugin(host), path_(host.data_directory() / "jobs.sqlite3"), db_(path_),
          delay_(config.value("delay_ms", 100)) {
        db_.verify_identity(host.instance_id(), id);
        db_.exec("CREATE TABLE IF NOT EXISTS work(id TEXT PRIMARY KEY,body TEXT NOT NULL)");

        host.provide("action.background");
    }

    // 向全部后台观察线程请求协作式停止。
    void quiesce() {
        for (auto& [key, observer] : observers_) {
            (void)key;
            observer.thread.request_stop();
        }
    }

    // 请求停止后台观察，并通过释放线程对象等待退出。
    void stop() {
        quiesce();
        observers_.clear();
    }

    // 模拟后台作业注册、执行交接、结果查询、重新连接和取消。
    // 步骤：回收已结束观察线程 → 消费注册回执并持久化工作 → 按动作或作业控制方法分派 → 回传交接或查询结果。
    void on_event(const Json& event) {
        std::erase_if(observers_, [](const auto& item) { return item.second.done->load(); });

        if (event.at("type") == "reply") {
            const auto call = event.at("call_id").get<uint64_t>();
            if (!event.at("error").is_null()) {
                pending_.erase(call);
                host.fail(call, event.at("error"));
                return;
            }

            const auto job = event.at("result");
            const auto key = job.at("id").get<std::string>();
            {
                dfy::Transaction transaction(db_);
                if (load(db_, key).is_null())
                    save(db_, {{"id", key}, {"due", clock_ms() + delay_}, {"state", "running"},
                                  {"value", pending_.at(call).at("value")}, {"result", nullptr}});
                transaction.commit();
            }

            pending_.erase(call);
            observe(key);
            host.complete(call, {{"type", "job_started"}, {"job_id", key}});
            return;
        }

        if (event.at("type") != "call")
            return;
        Call call(event);
        if (call.method == "execute" || call.method == "readiness" || call.method == "query_operation") {
            const ActionInvocation action(call);
            call.input = action.arguments;
            if (call.method == "query_operation")
                call.input["id"] = action.operation_id;
        }

        if (call.method == "readiness")
            host.complete(call.id, {{"available", true}});
        else if (call.method == "execute") {
            pending_[call.id] = call.input;
            host.register_job(call.id, "job", call.context.at("operation_id"));
        } else if (call.method == "query_operation")
            host.complete(call.id, outcome(Json::object(), "unknown", "unknown"));
        else {
            const auto key = call.input.at("id").get<std::string>();
            auto work = load(db_, key);
            if (call.method == "query_job")
                host.complete(call.id, work.is_null() ? Json{{"state", "interrupted"}} : work);
            else if (call.method == "reattach_job") {
                if (work.is_null())
                    throw std::runtime_error("JOB_NOT_FOUND");
                observe(key);
                host.complete(call.id, {{"attached", true}});
            } else if (call.method == "cancel_job") {
                if (!work.is_null()) {
                    {
                        dfy::Transaction transaction(db_);
                        work = load(db_, key);
                        if (work.at("state") != "completed") {
                            work["state"] = "completed";
                            work["result"] = outcome(Json::object(), "none", "cancelled");
                            save(db_, work);
                        }
                        transaction.commit();
                    }

                    host.job_result(key, 1, work.at("result"));
                }
                host.complete(call.id, {{"accepted", true}});
            } else
                throw std::runtime_error("METHOD_UNSUPPORTED");
        }
    }
};
DFY_PLUGIN(Background)
