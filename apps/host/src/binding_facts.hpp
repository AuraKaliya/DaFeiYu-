#pragma once
#include <dafeiyu/plugin_agents.hpp>
#include <deque>
#include <map>
#include <nlohmann/json.hpp>

namespace dfy::app {
// 单槽绑定内的已接纳事实。调用者先在副本准备，实际运行准入后才发布。
class BindingFacts {
  public:
    // 用原帧及实际请求语义判断是否已接纳，冲突不得重放。
    std::uint64_t prior(const plugins::SubagentInput& input) const;
    // 合入字段补丁，生成当前模型所见的数据及原始基线标识。
    std::string apply(const plugins::SubagentInput& input, std::uint64_t request, bool full);
    // 取已合入增量的状态采样，定义和读取权限由实例宿主提供。
    std::string status_samples() const;
    // 取得当前已准入绑定的代次。
    std::uint64_t epoch() const noexcept { return epoch_; }
    // 查询原作用域地址，供运行状态回执关联。
    const std::string& scope() const noexcept { return scope_; }
  private:
    struct Admission { std::uint64_t epoch,sequence,request; std::string digest; };
    std::uint64_t epoch_ = 0, sequence_ = 0;
    std::string scope_;
    nlohmann::json fields_ = nlohmann::json::object();
    std::deque<Admission> admitted_;
};

// 原工作按代次保留接纳、实效和退出，不持有执行入口。
class BindingWorks {
  public:
    // 幂等接纳工作阶段，过期代次只可推进原先已登记的工作。
    std::uint64_t settle(std::uint64_t current_epoch, const plugins::SubagentWorkFact& fact);
    // 按本帧剩余字节投影本代次的工作身份、实效和退出，优先展示较新及在途的正文。
    nlohmann::json view(std::uint64_t epoch, std::uint64_t after, std::size_t byte_limit = 128*1024) const;
    // 展开当前留存修订的原文，已归档工作返回原阶段和指纹，窗口外返回空。
    std::optional<std::string> read(std::uint64_t epoch, std::uint64_t receipt) const;
    // 所有保留工作均已退出且领域最终确认实效时，可回收退役绑定的只读窗口。
    bool finalized() const noexcept;
    // 返回最近一次真实接纳的工作回执水位，供冻结增量。
    std::uint64_t watermark() const noexcept { return next_; }
  private:
    struct Record { std::uint32_t phase; std::string content; std::uint64_t receipt, revision; bool final_effect; };
    using Key = std::pair<std::uint64_t,std::string>;
    struct Work {
        std::vector<Record> revisions;
        std::uint64_t sequence;
        bool exited = false, final_effect = false;
    };
    // 有序原件在重传窗口内保留每版指纹和接纳号，窗口外由首次接纳水位拒绝旧号。
    std::deque<std::pair<Key,Work>> archived_;
    std::map<Key,Work> records_;
    std::uint64_t admission_epoch_ = 0, admission_sequence_ = 0;
    // 按修订和原件校验重传；归档记录使用规范正文指纹作比较。
    static std::uint64_t prior(const Work& work, const plugins::SubagentWorkFact& fact, const std::string& content);
    // 容量紧张时只归档已声明最终实效且实际退出的有序工作。
    void admit(const Key& key, Record first, std::uint64_t sequence);
    std::uint64_t next_ = 0;
};
}
