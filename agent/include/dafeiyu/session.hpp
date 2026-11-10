#pragma once
#include <dafeiyu/model_client.hpp>

namespace dfy::agent {
// 单线程同步会话，仅保存当前进程内已成功完成的问答。
class Session {
  public:
    // 保存已由 load_config 校验的模型配置，开始一个空会话。
    explicit Session(model::Config config);

    // 携带历史请求一次回答，仅在完整成功后提交本轮问答；失败时抛出异常。
    std::string ask(const std::string& prompt);

    // 清空当前会话上下文，后续提问从空历史开始。
    void clear();

  private:
    model::Config config_;
    std::vector<model::Message> history_;
};
}
