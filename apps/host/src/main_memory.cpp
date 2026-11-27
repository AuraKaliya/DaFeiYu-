#include "main_messages.hpp"
#include "application.hpp"
#include <algorithm>

namespace dfy::app {
using Json = nlohmann::json;
// 按工具执行的实际结果返回回执，区分写入、只读与拒绝。
std::unique_ptr<agent::ToolWork> MainMessages::memory_receipt(Json value, agent::ToolEffect effect, bool ok) {
    return agent::completed_tool({ok,effect,value.dump(),ok ? agent::ToolError::none : agent::ToolError::failed});
}
namespace {
// 冻结有序的本轮证据；工具回执与普通会话历史只记录查询结果的数量。
agent::DecisionStatusSource matches_source(std::string_view profile, std::uint64_t turn, const Json& result) {
    static const auto schema = [] {
        Json fields = Json::object();
        for (const auto key : {"reference","topic","source","quote"}) fields[key] = {{"type","string"}};
        fields["utcMs"] = {{"type","integer"}};
        for (const auto key : {"standing","conclusion","latestReference","previousReference"}) fields[key] = {{"type","string"}};
        fields["mentions"] = {{"type","integer"}};
        Json item{{"type","object"},{"properties",fields},{"required",{"reference","topic","source","quote","utcMs"}},{"additionalProperties",false}};
        Json value{{"type","object"},{"properties",{{"items",{{"type","array"},{"items",item}}},{"limited",{{"type","boolean"}}},
            {"reviewDue",{{"type","string"}}}}},
            {"required",{"items","limited"}},{"additionalProperties",false}};
        return Json::array({{{"key","memory.matches"},{"version",2},{"description","获准的原话及有依据的复盘结论，按匹配顺序列出"},
            {"defaultAttention",{{"mode","explicit"}}},{"snapshotRead",false},{"valueSchema",value}}}).dump();
    }();
    return {"memory",schema,Json::array({{{"key","memory.matches"},{"version",2},{"subject",profile},{"fact",turn},{"value",result}}}).dump()};
}
}

// 将语义判断交给主 Agent，以工具契约说明长期价值、当轮用途和排除情形。
void MainMessages::register_memory_tools() {
    reply_tools_->add({"main/memory_note",app_.mailbox_host().identity,2,{"memory_note",
        "主人直接陈述可复用的长期偏好、持续约束、未来承诺、影响后续安排的经历或对这些事实的更正时，选简短稳定的主题，"
        "摘录当前原话中的依据 excerpt。只有主人确认且未来仍有用时 scope 取 ongoing。涉及拒绝保存、敏感秘密、转述、猜测、寒暄或一次性要求时直接交谈。"
        "收录对象是当前整条真人原话，混有不宜保存的内容时跳过。仅在收到 saved 回执后确认收录。"
        "回执含 reviewDue 表示已有同主题原件，宜用 memory_search 的 originals 对照。同一事项才复盘，主题相同本身不足以合并。",
        R"({"type":"object","properties":{"topic":{"type":"string"},"excerpt":{"type":"string"},"kind":{"type":"string","enum":["preference","constraint","commitment","event","correction"]},"scope":{"type":"string","enum":["ongoing","one_time","unconfirmed","private"]}},"required":["topic","excerpt","kind","scope"],"additionalProperties":false})"},
        true,true,{},{},1,512,true},[this](const agent::ToolInvocation& call){return consider_memory(call);});
    reply_tools_->add({"main/memory_search",app_.mailbox_host().identity,3,{"memory_search",
        "当答案需要以前的约定、偏好或个人事实，且当前对话依据不足时检索。普通常识、寒暄、当前原话足够时直接回应。"
        "topics 填一至三个主题词，可含同义词。need 按缺少的依据填 single 或 multiple，已有依据填 context_sufficient，常识填 general。"
        "count 按回答所需给出，单个事实取一条，对照时最多三条。"
        "每轮一次。view 默认 current，问变更过程选 history，比较具体原话选 originals，此时 topics 也可填已知原件号。查阅 memory.matches；"
        "current 标明当前依据，superseded 是当时的说法，needs_review 尚有更正待核对。回答时说明适用版本，追溯时引用原话和 reference。"
        "mentions 是获准原话数。previousReference 可追溯上次更正依据。无 standing 的条目是尚未整理的原话。"
        "reviewDue 表示有待比较的原话。回顾自己实际作过的选择用 episodes，topics 可填主题或投递原件号。"
        "why 查现在对过去的解释，change 查相似情境下实际换过的选择。用一个主题或当时那场的投递号查配套依据，need 用 multiple，count 给三。"
        "从 memory.experiences 区分当时的感受与后来推测，说明哪几次经历支持判断并引用 reference。limited 表示材料未全部装入。",
        R"({"type":"object","properties":{"topics":{"type":"array","items":{"type":"string"}},"count":{"type":"integer"},"need":{"type":"string","enum":["single","multiple","context_sufficient","general"]},"view":{"type":"string","enum":["current","history","originals","episodes","why","change"]}},"required":["topics","count","need"],"additionalProperties":false})"},
        false,true,{},{},2,512,true},[this](const agent::ToolInvocation& call){return search_memory(call);});
    reply_tools_->add({"main/memory_reflect",app_.mailbox_host().identity,1,{"memory_reflect",
        "同一事项反复提到、主人明确更改偏好，或追问前后依据时，比较本轮已查回的两条原话。"
        "prior、later 填从早到晚的 reference，两段 excerpt 各取相应原话中保留适用条件的完整意思，已整理的依据沿用其 conclusion。"
        "同一件事的重述选 confirms；明确替代旧约定选 replaces。不同事项、临时例外或矛盾未明时保持分开，需要时追问。",
        R"({"type":"object","properties":{"prior":{"type":"string"},"later":{"type":"string"},"priorExcerpt":{"type":"string"},"laterExcerpt":{"type":"string"},"relation":{"type":"string","enum":["confirms","replaces"]}},"required":["prior","later","priorExcerpt","laterExcerpt","relation"],"additionalProperties":false})"},
        true,true,{},{},3,512,true},[this](const agent::ToolInvocation& call){return reflect_memory(call);});
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
    Json receipt{{"state","saved"},{"reference",ref}};
    try {
        const auto kind = args.at("kind").get<std::string>();
        const auto result = store_->change_memory(ref,args.at("topic").get<std::string>(),excerpt,kind);
        memory_revision_ = result.revision;
        if (result.review_due) receipt["reviewDue"] = kind=="correction" ? "correction" : "compare";
    } catch (const std::invalid_argument&) {
        return memory_receipt({{"state","invalid_source"}},agent::ToolEffect::not_applied,false);
    }
    return memory_receipt(std::move(receipt),agent::ToolEffect::applied);
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
        const auto topics = args.at("topics").get<std::vector<std::string>>();
        const auto view = args.value("view",std::string{"current"});
        const bool own = view=="episodes" || view=="why" || view=="change";
        const auto take = count>=limit ? limit : count.get<unsigned>();
        const auto result = own ? store_->search_experience(topics,take,view) : store_->search_memory(topics,take,view);
        const auto size = result.at("items").size();
        search_result_ = own ? experience_source(call.attempt.step.run.sequence,result) :
            matches_source(store_->conversation(),call.attempt.step.run.sequence,result);
        search_step_ = call.attempt.step.sequence;
        return memory_receipt({{"state",size ? "ready" : result.at("limited")==true ? "limited" : "no_match"},
            {"count",size},{"limited",result.at("limited")}},agent::ToolEffect::not_applied);
    } catch (const std::invalid_argument&) {
        return memory_receipt({{"state","invalid_query"}},agent::ToolEffect::not_applied,false);
    }
}

// 限制比较到本轮检索给出的原件，写盘时按当前授权重新验证引用和版本。
std::unique_ptr<agent::ToolWork> MainMessages::reflect_memory(const agent::ToolInvocation& call) {
    const auto args = Json::parse(call.call.arguments);
    if (!owner_memory_turn_ || !search_result_ || call.attempt.step.sequence<=search_step_)
        return memory_receipt({{"state","unseen_evidence"}},agent::ToolEffect::not_applied,false);
    const auto samples = Json::parse(search_result_->samples);
    if (samples.at(0).at("key")!="memory.matches")
        return memory_receipt({{"state","unseen_evidence"}},agent::ToolEffect::not_applied,false);
    const auto& items = samples.at(0).at("value").at("items");
    for (const auto key : {"prior","later"})
        if (std::none_of(items.begin(),items.end(),[&](const auto& item){return item.at("reference")==args.at(key);}))
            return memory_receipt({{"state","unseen_evidence"}},agent::ToolEffect::not_applied,false);
    try {
        store_->reflect_memory(args);
    } catch (const std::invalid_argument&) {
        return memory_receipt({{"state","invalid_comparison"}},agent::ToolEffect::not_applied,false);
    }
    return memory_receipt({{"state","reviewed"},{"relation",args.at("relation")},{"prior",args.at("prior")},{"later",args.at("later")}},
        agent::ToolEffect::applied);
}

// 将管理操作提交到同一持久身份；撤权后停止可能带有旧正文的在途私聊。
void MainMessages::change_memory(std::string_view identity, const std::optional<std::string>& topic) {
    const auto result = store_->change_memory(identity,topic);
    memory_revision_ = result.revision;
    if (result.changed && !topic) {
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
