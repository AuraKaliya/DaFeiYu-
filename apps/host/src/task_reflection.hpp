#pragma once
#include "message_store.hpp"
#include "persona.hpp"
#include <dafeiyu/async_model.hpp>

namespace dfy::app {
class RunLog;
// 大肥鱼本体的单次空闲推断，共用模型和人格，独立于前台聊天历史。
class TaskReflection {
  public:
    // 绑定原档案、配置、时钟和唯一的后台调用槽。
    TaskReflection(std::shared_ptr<MessageStore> store, model::Config config, model::Backend backend,
        PersonaProfile persona, agent::ToolClock clock, RunLog* trace);
    // 新任务或撤权后重新检查队列，旧的在途推断不能写回。
    void interrupt();
    // 来信入箱后重新检查待办，已经在整理的独立材料可继续。
    void notify() noexcept {dirty_=true;}
    // 推进模型实际退出，连续空闲后才取新材料或写回。
    void advance(bool idle);
    // 请求协作停止并报告后台调用是否实际退出。
    bool close();
  private:
    std::shared_ptr<MessageStore> store_;
    model::Config config_;
    PersonaProfile persona_;
    agent::ToolClock clock_;
    RunLog* trace_;
    model::AsyncClient client_;
    std::shared_ptr<model::ModelRequest> request_;
    std::optional<nlohmann::json> batch_;
    std::optional<agent::ToolTime> ready_;
    bool dirty_ = true;
    unsigned next_queue_ = 0;
    std::string request_scope_;
};
}
