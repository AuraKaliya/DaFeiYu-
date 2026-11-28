#pragma once
#include "task_memory_fixture.hpp"
#include <map>

namespace dfy::tests {
// 固定后端按原件提议知识，主人的批准则从独立的管理命令进入。
struct KnowledgeProbe {
    using Json=nlohmann::json;
    std::map<std::string,std::string> references;
    Json row;
    unsigned step=0;
    std::string reply;
    std::string document_text;
    // 将测试别名解析成此次档案实际交付的原件身份。
    std::string resolve(const std::string& alias) const;
    // 在普通主人来信中发出一次读取及可选的草案提议，并核对真实答复。
    void chat(app::Application& host,const Json& input,unsigned id);
    // 固定每一步的提议，检查实际进入后端的知识或来源。
    model::Response answer(const model::Request& input);
    // 通过原管理分派器执行主人命令并检查成功、拒绝或审计字段。
    void admin(app::Application& host,const Json& input,const std::filesystem::path& directory);
    // 核对实际装配的知识版本、适用条件、出处与受众可见范围。
    void verify(const Json& items,const Json& wanted,bool owner) const;
    // 对照真实工具 Trace 和原信身份，检查跨工具事实、意见与反例。
    void verify_comparison(const Json& items);
    std::map<std::string,std::vector<Json>> traces;
};
}
