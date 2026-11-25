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
    // 新绑定的模型只能看到本代次的在途工作、未确认实效和最近结算。
    nlohmann::json view(std::uint64_t epoch) const;
  private:
    struct Record { std::uint32_t phase; std::string content; std::uint64_t receipt; };
    using Key = std::pair<std::uint64_t,std::string>;
    std::map<Key,std::vector<Record>> records_;
    std::uint64_t next_ = 0;
};
}
