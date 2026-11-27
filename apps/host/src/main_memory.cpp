#include "main_messages.hpp"
#include "application.hpp"

namespace dfy::app {
using Json = nlohmann::json;

// 将管理操作提交到同一持久身份；撤权后停止可能带有旧正文的在途私聊。
void MainMessages::change_memory(std::string_view identity, const std::optional<std::string>& topic) {
    const auto [revision,changed] = store_->change_memory(identity,topic);
    memory_revision_ = revision;
    if (changed && !topic && app_.conversation().busy() && memory_binding_.starts_with("main/owner/"))
        app_.conversation().cancel();
}

// 将读出的原话作为带来源的数据，沿正式决策尾注和上下文预算送模。
// 步骤：按受众及修订隔离历史、投递和快照 → 匹配私聊请求 → 绑定原事件与逐步采样。
void MainMessages::prepare_memory(agent::RunOptions& options, DialogueAudience audience, const std::string& prompt) {
    memory_revision_ = store_->memory_revision();
    if (!memory_revision_) return;
    auto& status = *options.decision_status;
    const bool owner_dialogue = audience==DialogueAudience::owner;
    status.binding = std::string(owner_dialogue ? "main/owner/" : "main/other/") + std::to_string(memory_revision_);
    if (memory_binding_ != status.binding) {
        const auto previous = app_.conversation().status();
        clear_context(previous ? previous->turn : 0);
        memory_binding_ = status.binding;
    }
    constexpr std::string_view prefix = "回想 ";
    if (!owner_dialogue) return;
    std::string selector;
    if (incoming_memory_) selector = store_->conversation()+"/"+std::to_string(*incoming_memory_);
    else if (prompt.starts_with(prefix) && prompt.size()<=prefix.size()+128) selector = prompt.substr(prefix.size());
    else return;
    const auto original = store_->recall_memory(selector);
    if (!original) return;

    static const auto schema = [] {
        Json properties = Json::object();
        for (const auto key : {"conversation","message","reference","source","submission","quote"})
            properties[key] = {{"type","string"}};
        return Json::array({{{"key","memory.original"},{"version",1},{"description","已收录并获准在主人私聊引用的原话；reference 可查回或撤权"},
            {"defaultAttention",{{"mode","explicit"}}},{"valueSchema",{{"type","object"},{"properties",properties},
            {"required",{"conversation","message","reference","source","submission","quote"}},{"additionalProperties",false}}}}}).dump();
    }();
    const auto utc = original->envelope.value("sourceUtcMs",original->utc_ms);
    const auto identity = store_->conversation()+"/"+std::to_string(original->id);
    const Json value{{"conversation",store_->conversation()},{"message",std::to_string(original->id)},
        {"reference",identity},{"source",original->source},{"submission",original->submission},{"quote",original->envelope.at("text")}};
    agent::DecisionStatusSource memory{"memory",schema,Json::array({{{"key","memory.original"},{"version",1},
        {"subject",store_->conversation()},{"fact",original->id},{"sampledUtcMs",utc},{"value",value}}}).dump()};
    status.sample = [channel=weak_from_this(),revision=memory_revision_,identity,other=std::move(status.sample),memory=std::move(memory)] {
        const auto current = channel.lock();
        if (!current || (current->memory_revision_!=revision && !current->store_->recall_memory(identity)))
            throw std::logic_error("memory: access changed");
        auto sources = other ? other() : std::vector<agent::DecisionStatusSource>{};
        sources.push_back(memory);
        return sources;
    };
}
}
