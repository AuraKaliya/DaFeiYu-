#pragma once
#include <dafeiyu/run_capture.hpp>
#include <map>
#include <nlohmann/json_fwd.hpp>

namespace dfy::app {
// 单个会话原件代次的只读契约索引；成功提交的模型请求才能成为后续引用来源。
class ToolContractTrace {
  public:
    // 以本次实际工具顺序准备变动和原契约引用，尚未确认模型提交。
    nlohmann::json prepare(const agent::ModelCapture& capture);
    // 仅在同一尝试确已提交后发布新基线。
    void submitted(agent::AttemptId attempt) noexcept;
  private:
    struct Known {
        std::string digest;
        agent::AttemptId origin{};
        std::size_t index = 0;
        dfy::runtime::DomainId instance = 0;
        bool available = false;
    };
    struct State {
        dfy::runtime::DomainId session = 0;
        std::uint64_t epoch = 0;
        std::map<std::string,Known> known;
    };
    State committed_, next_;
    agent::AttemptId pending_{};
};
}
