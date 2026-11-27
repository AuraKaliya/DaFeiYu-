#include "main_messages.hpp"
#include "application.hpp"

namespace dfy::app {
using Json = nlohmann::json;
namespace {
// 按工具执行的实际结果返回回执，区分写入、只读与拒绝。
std::unique_ptr<agent::ToolWork> memory_receipt(Json value, agent::ToolEffect effect, bool ok = true) {
    return agent::completed_tool({ok,effect,value.dump(),ok ? agent::ToolError::none : agent::ToolError::failed});
}
// 冻结有序的本轮证据；工具回执与普通会话历史只记录查询结果的数量。
agent::DecisionStatusSource matches_source(std::string_view profile, std::uint64_t turn, const Json& result) {
    static const auto schema = [] {
        Json fields = Json::object();
        for (const auto key : {"reference","topic","source","quote"}) fields[key] = {{"type","string"}};
        fields["utcMs"] = {{"type","integer"}};
        Json item{{"type","object"},{"properties",fields},{"required",{"reference","topic","source","quote","utcMs"}},{"additionalProperties",false}};
        Json value{{"type","object"},{"properties",{{"items",{{"type","array"},{"items",item}}},{"limited",{{"type","boolean"}}}}},
            {"required",{"items","limited"}},{"additionalProperties",false}};
        return Json::array({{{"key","memory.matches"},{"version",1},{"description","本轮按相关主题查回的主人原话，按匹配顺序列出"},
            {"defaultAttention",{{"mode","explicit"}}},{"snapshotRead",false},{"valueSchema",value}}}).dump();
    }();
    return {"memory",schema,Json::array({{{"key","memory.matches"},{"version",1},{"subject",profile},{"fact",turn},{"value",result}}}).dump()};
}
}

// 将语义判断交给主 Agent，以工具契约说明长期价值、当轮用途和排除情形。
void MainMessages::register_memory_tools() {
    reply_tools_->add({"main/memory_note",app_.mailbox_host().identity,1,{"memory_note",
        "主人直接陈述可复用的长期偏好、持续约束、未来承诺、影响后续安排的经历或对这些事实的更正时，选简短稳定的主题，"
        "摘录当前原话中的依据 excerpt。只有主人确认且未来仍有用时 scope 取 ongoing。涉及拒绝保存、敏感秘密、转述、猜测、寒暄或一次性要求时直接交谈。"
        "收录对象是当前整条真人原话，混有不宜保存的内容时跳过。仅在收到 saved 回执后确认收录。",
        R"({"type":"object","properties":{"topic":{"type":"string"},"excerpt":{"type":"string"},"kind":{"type":"string","enum":["preference","constraint","commitment","event","correction"]},"scope":{"type":"string","enum":["ongoing","one_time","unconfirmed","private"]}},"required":["topic","excerpt","kind","scope"],"additionalProperties":false})"},
        true,true,{},{},1,512,true},[this](const agent::ToolInvocation& call){return consider_memory(call);});
    reply_tools_->add({"main/memory_search",app_.mailbox_host().identity,1,{"memory_search",
        "当答案需要以前的约定、偏好或个人事实，且当前对话依据不足时检索。普通常识、寒暄、当前原话足够时直接回应。"
        "topics 填一至三个主题词，可含同义词。need 按缺少的依据填 single 或 multiple，已有依据填 context_sufficient，常识填 general。"
        "count 按回答所需给出，单个事实取一条，对照时最多三条。"
        "每轮一次，查阅下一步宿主状态 memory.matches 的有序原件。limited 表示部分命中未装入，无依据时说明或追问。",
        R"({"type":"object","properties":{"topics":{"type":"array","items":{"type":"string"}},"count":{"type":"integer"},"need":{"type":"string","enum":["single","multiple","context_sufficient","general"]}},"required":["topics","count","need"],"additionalProperties":false})"},
        false,true,{},{},2,512,true},[this](const agent::ToolInvocation& call){return search_memory(call);});
}

// 只按当前主人原话核实模型引用的片段，临时或未确认的判断不产生长期索引。
std::unique_ptr<agent::ToolWork> MainMessages::consider_memory(const agent::ToolInvocation& call) {
    const auto args = Json::parse(call.call.arguments);
    if (args.at("scope")!="ongoing")
        return memory_receipt({{"state","skipped"}},agent::ToolEffect::not_applied);
    const auto excerpt = args.at("excerpt").get<std::string>();
    if (!owner_memory_turn_ || parent_.first!=call.attempt.step.run ||
        excerpt.find_first_not_of(" \r\n\t")==excerpt.npos || excerpt.size()>2048)
        return memory_receipt({{"state","invalid_source"}},agent::ToolEffect::not_applied,false);
    const auto ref = store_->conversation()+"/"+std::to_string(parent_.second);
    try {
        memory_revision_ = store_->change_memory(ref,args.at("topic").get<std::string>(),excerpt).first;
    } catch (const std::invalid_argument&) {
        return memory_receipt({{"state","invalid_source"}},agent::ToolEffect::not_applied,false);
    }
    return memory_receipt({{"state","saved"},{"reference",ref}},agent::ToolEffect::applied);
}

// 按实际受众和本轮额度签发一次检索；无命中也有确定回执。
std::unique_ptr<agent::ToolWork> MainMessages::search_memory(const agent::ToolInvocation& call) {
    if (!owner_memory_turn_ || search_result_)
        return memory_receipt({{"state","unavailable"}},agent::ToolEffect::not_applied,false);
    const auto args = Json::parse(call.call.arguments);
    const auto& need = args.at("need");
    if (need=="context_sufficient" || need=="general")
        return memory_receipt({{"state","skipped"}},agent::ToolEffect::not_applied);
    const auto& count = args.at("count");
    if (count<=0) return memory_receipt({{"state","invalid_query"}},agent::ToolEffect::not_applied,false);
    try {
        const unsigned limit = need=="single" ? 1u : 3u;
        const auto result = store_->search_memory(args.at("topics").get<std::vector<std::string>>(),count>=limit ? limit : count.get<unsigned>());
        const auto size = result.at("items").size();
        search_result_ = matches_source(store_->conversation(),call.attempt.step.run.sequence,result);
        return memory_receipt({{"state",size ? "ready" : result.at("limited")==true ? "limited" : "no_match"},
            {"count",size},{"limited",result.at("limited")}},agent::ToolEffect::not_applied);
    } catch (const std::invalid_argument&) {
        return memory_receipt({{"state","invalid_query"}},agent::ToolEffect::not_applied,false);
    }
}

// 将管理操作提交到同一持久身份；撤权后停止可能带有旧正文的在途私聊。
void MainMessages::change_memory(std::string_view identity, const std::optional<std::string>& topic) {
    const auto [revision,changed] = store_->change_memory(identity,topic);
    memory_revision_ = revision;
    if (changed && !topic) {
        search_result_.reset();
        if (app_.conversation().busy() && owner_memory_turn_) app_.conversation().cancel();
    }
}

// 将读出的原话作为带来源的数据，沿正式决策尾注和上下文预算送模。
// 步骤：按受众及修订隔离历史、投递和快照 → 匹配私聊请求 → 绑定原事件与逐步采样。
void MainMessages::prepare_memory(agent::RunOptions& options, DialogueAudience audience, const std::string& prompt) {
    memory_revision_ = store_->memory_revision();
    search_result_.reset();
    owner_memory_turn_ = audience==DialogueAudience::owner;
    if (!memory_revision_ && !reply_tools_) return;
    auto& status = *options.decision_status;
    if (memory_revision_) status.binding = std::string(owner_memory_turn_ ? "main/owner/" : "main/other/") + std::to_string(memory_revision_);
    if (memory_binding_ != status.binding) {
        const auto previous = app_.conversation().status();
        clear_context(previous ? previous->turn : 0);
        memory_binding_ = status.binding;
    }
    constexpr std::string_view prefix = "回想 ";
    if (!owner_memory_turn_) return;
    status.sample = [channel=weak_from_this(),other=std::move(status.sample)] {
        auto sources = other ? other() : std::vector<agent::DecisionStatusSource>{};
        const auto current = channel.lock();
        if (current && current->search_result_) sources.push_back(*current->search_result_);
        return sources;
    };
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
