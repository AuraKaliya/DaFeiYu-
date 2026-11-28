#include "message_store_db.hpp"
#include "tone_memory.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <future>
#include <thread>

namespace dfy::app {
using namespace message_db;
using Json=nlohmann::json;
namespace {
// 缓存允许固定替身与生产向量，拒绝空向量、零模和非有限分量。
bool valid(const std::vector<float>& vector) {
    return !vector.empty() && vector.size()<=1024 &&
        std::ranges::all_of(vector,[](float value){return std::isfinite(value);}) &&
        std::ranges::any_of(vector,[](float value){return value!=0;});
}

struct Query {
    std::string text;
    std::promise<std::vector<float>> result;
    std::stop_source stop;
};

class ToneEncoder {
  public:
    // 单根后台线程拥有编码会话，关闭时等待该次计算实际退出。
    ToneEncoder(std::shared_ptr<MessageStore> store,std::function<model::LocalEncoder()> open,
        std::chrono::milliseconds budget) : budget_(budget),worker_(
        [this,store=std::move(store),open=std::move(open)](std::stop_token stop){run(*store,open,stop);}) {}

    // 只有准备完成且无正在退出的查询才接纳来信，超时取消并丢弃本次结果。
    // 步骤：核对输入与空槽 → 交付单项查询 → 等待首包预算 → 取回向量或申请停止。
    std::vector<float> query(std::string_view text) {
        if (text.empty() || text.size()>8192 || !scope()) return {};
        auto work=std::make_shared<Query>(); work->text=text;
        auto result=work->result.get_future();
        {
            std::lock_guard lock(mutex_);
            if (busy_) return {};
            busy_=true; pending_=work;
        }
        ready_.notify_one();
        if (result.wait_for(budget_)!=std::future_status::ready) {work->stop.request_stop(); return {};}
        return result.get();
    }

    // 返回本次启动采用的不可变范围向量。
    std::shared_ptr<const std::vector<float>> scope() const {return scope_.load();}
    // 返回当前加载状态，供管理命令及固定回放查询。
    std::string_view status() const {return state_.load();}

  private:
    // 先取同版范围缓存，再依次处理仍有预算的来信。
    // 步骤：打开编码器 → 读取或生成范围 → 公布可用性 → 处理有限的查询直至停止。
    void run(MessageStore& store,const std::function<model::LocalEncoder()>& open,std::stop_token stop) noexcept {
        try {
            const auto encoder=open();
            if (!encoder.encode || encoder.fingerprint.empty() || stop.stop_requested()) {state_="unavailable"; return;}
            auto vector=store.cached_tone_scope(casual_tone_scope,encoder.fingerprint);
            const bool cached=!vector.empty();
            if (!cached) {
                vector=encoder.encode(casual_tone_scope,stop);
                if (!valid(vector) || stop.stop_requested()) {state_="unavailable"; return;}
                store.save_tone_scope(casual_tone_scope,encoder.fingerprint,vector);
            }
            scope_=std::make_shared<const std::vector<float>>(std::move(vector));
            state_=cached ? "ready_cached" : "ready_encoded";
            while (!stop.stop_requested()) {
                std::shared_ptr<Query> work;
                {
                    std::unique_lock lock(mutex_);
                    if (!ready_.wait(lock,stop,[this]{return bool(pending_);})) return;
                    work=std::move(pending_);
                }
                std::stop_callback cancel(stop,[&work]{work->stop.request_stop();});
                std::vector<float> result;
                try {
                    if (!work->stop.stop_requested()) result=encoder.encode(work->text,work->stop.get_token());
                } catch (const std::exception&) {}
                {
                    std::lock_guard lock(mutex_);
                    busy_=false;
                }
                work->result.set_value(work->stop.stop_requested() ? std::vector<float>{} : std::move(result));
            }
        } catch (const std::exception&) {scope_=nullptr; state_="unavailable";}
    }

    const std::chrono::milliseconds budget_;
    std::mutex mutex_;
    std::condition_variable_any ready_;
    bool busy_=false;
    std::shared_ptr<Query> pending_;
    std::atomic<const char*> state_{"loading"};
    std::atomic<std::shared_ptr<const std::vector<float>>> scope_;
    std::jthread worker_;
};
}

// 若 SQLite 里已有同版合法向量则直接复用，旧版和无效缓存均可重建。
std::vector<float> MessageStore::cached_tone_scope(std::string_view scope,std::string_view encoder) const {
    std::lock_guard lock(state_->mutex);
    Statement get(state_->db,"SELECT components FROM tone_scope_vector WHERE slot=1 AND scope=? AND encoder=?");
    get.bind(1,scope); get.bind(2,encoder);
    if (!get.next()) return {};
    try {
        auto vector=Json::parse(get.text(0)).get<std::vector<float>>();
        if (valid(vector)) return vector;
    } catch (const Json::exception&) {}
    return {};
}

// 缓存仅由已准备的编码实例更新，事务中的单行原子替换不推进记忆授权修订。
void MessageStore::save_tone_scope(std::string_view scope,std::string_view encoder,const std::vector<float>& components) {
    if (!valid(components) || encoder.empty() || encoder.size()>128 || scope.empty())
        throw std::invalid_argument("embedding: invalid scope cache");
    std::lock_guard lock(state_->mutex);
    Statement save(state_->db,"INSERT INTO tone_scope_vector VALUES(1,?,?,?) "
        "ON CONFLICT(slot) DO UPDATE SET scope=excluded.scope,encoder=excluded.encoder,components=excluded.components");
    save.bind(1,scope); save.bind(2,encoder); save.bind(3,Json(components).dump()); save.next();
}

// 将后台编码实例接到已有语义补漏选择器，缺配置时保持原对话路径。
ToneRecallOptions prepare_tone_encoder(std::shared_ptr<MessageStore> store,std::function<model::LocalEncoder()> open,
    double minimum_similarity,std::chrono::milliseconds query_budget) {
    if (!store || !open || !(minimum_similarity>0 && minimum_similarity<=1) || query_budget.count()<=0) return {};
    const auto worker=std::make_shared<ToneEncoder>(std::move(store),std::move(open),query_budget);
    return {.encode_query=[worker](std::string_view text){return worker->query(text);},
        .minimum_similarity=minimum_similarity,.prepared_scope=[worker]{return worker->scope();},
        .encoder_state=[worker]{return worker->status();}};
}
}
