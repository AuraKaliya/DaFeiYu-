#include "main_messages.hpp"
#include "task_reflection.hpp"
#include "knowledge.hpp"
#include "application.hpp"
#include <algorithm>
#include <utility>

namespace dfy::app {
using Json = nlohmann::json;
// 按工具执行的实际结果返回回执，区分写入、只读与拒绝。
std::unique_ptr<agent::ToolWork> MainMessages::memory_receipt(Json value, agent::ToolEffect effect, bool ok) {
    return agent::completed_tool({ok,effect,value.dump(),ok ? agent::ToolError::none : agent::ToolError::failed});
}
namespace {
// 冻结有序的本轮证据；工具回执与普通会话历史只记录查询结果的数量。
agent::DecisionStatusSource matches_source(std::string_view subject, std::uint64_t turn, const Json& result) {
    static const auto schema = [] {
        Json fields = Json::object();
        for (const auto key : {"reference","topic","source","quote"}) fields[key] = {{"type","string"}};
        fields["utcMs"] = {{"type","integer"}};
        for (const auto key : {"standing","conclusion","latestReference","previousReference"}) fields[key] = {{"type","string"}};
        fields["mentions"] = {{"type","integer"}};
        fields["nextOffset"] = {{"type","integer"}};
        Json item{{"type","object"},{"properties",fields},{"required",{"reference","topic","source","quote","utcMs"}},{"additionalProperties",false}};
        Json value{{"type","object"},{"properties",{{"items",{{"type","array"},{"items",item}}},{"limited",{{"type","boolean"}}},
            {"reviewDue",{{"type","string"}}}}},
            {"required",{"items","limited"}},{"additionalProperties",false}};
        return Json::array({{{"key","memory.matches"},{"version",2},{"description","获准的原话及有依据的复盘结论，按匹配顺序列出"},
            {"defaultAttention",{{"mode","explicit"}}},{"snapshotRead",false},{"valueSchema",value}}}).dump();
    }();
    return {"memory",schema,Json::array({{{"key","memory.matches"},{"version",2},{"subject",subject},{"fact",turn},{"value",result}}}).dump()};
}

// 只交付主题、次数与原件索引，深查仍需本轮调用原件读取。
agent::DecisionStatusSource theme_source(std::string_view profile,std::uint64_t turn,const Json& result) {
    static const auto schema=[] {
        const Json string{{"type","string"}};
        const Json item{{"type","object"},{"properties",{{"topic",string},{"mentions",{{"type","integer"}}},
            {"references",{{"type","array"},{"items",string}}}}},{"additionalProperties",false}};
        const Json value{{"type","object"},{"properties",{{"items",{{"type","array"},{"items",item}}},
            {"limited",{{"type","boolean"}}}}},{"additionalProperties",false}};
        return Json::array({{{"key","memory.themes"},{"version",1},{"description","主人的经历主题和原件索引。与当前来信相关时，可按号深查。"},
            {"defaultAttention",{{"mode","explicit"}}},{"snapshotRead",false},{"valueSchema",value}}}).dump();
    }();
    return {"memory",schema,Json::array({{{"key","memory.themes"},{"version",1},{"subject",profile},{"fact",turn},{"value",result}}}).dump()};
}

// 浅查后只允许展开刚选出的原件，不能借一次索引开启其他任意查询。
bool can_open_theme(const Json& sample,const std::vector<std::string>& topics) {
    if (sample.at("key")!="memory.themes" || topics.empty() || topics.size()>3) return false;
    const auto& items=sample.at("value").at("items");
    return std::ranges::all_of(topics,[&](const auto& reference){
        return std::ranges::any_of(items,[&](const auto& item){
            const auto& refs=item.at("references"); return std::find(refs.begin(),refs.end(),Json(reference))!=refs.end();
        });
    });
}

// 原件续页只接受刚送达的同一编号与下一段位置。
bool next_source_page(const Json& sample,std::string_view key,const std::string& reference,std::size_t offset) {
    if (sample.at("key").get_ref<const std::string&>()!=key) return false;
    const auto& items=sample.at("value").at("items");
    return items.size()==1 && items.front().at("reference")==reference &&
        items.front().contains("nextOffset") && items.front().at("nextOffset")==offset;
}

// 从刚查到的知识版打开引用原件，随后只沿原件签发的下一页位置续读。
bool can_open_document(const Json& sample,const std::string& reference,std::size_t offset) {
    if (sample.at("key")=="knowledge.published" && !offset) {
        return std::ranges::any_of(sample.at("value").at("items"),[&](const auto& item){
            return std::ranges::any_of(item.at("sources"),[&](const auto& source){
                return source.at("kind")=="external_document" && source.at("reference")==reference;
            });
        });
    }
    return next_source_page(sample,"knowledge.original",reference,offset);
}

// 只认刚送达目录中的完整库名，库名相同的其他条目不参与本次检索。
bool can_open_library(const Json& sample,const std::vector<std::string>& topics) {
    return sample.at("key")=="knowledge.libraries" && topics.size()==1 &&
        std::ranges::any_of(sample.at("value").at("items"),[&](const auto& item){return item.at("library")==topics.front();});
}

// 私有语气材料只能用于本步决策，历史状态查阅不再展开。
agent::DecisionStatusSource tone_source(std::string_view key,std::string_view description,Json value,
    std::initializer_list<const char*> text_fields) {
    Json fields{{"message",{{"type","integer"}}},{"reference",{{"type","string"}}}};
    for (const auto name : text_fields) fields[name]={{"type","string"}};
    Json schema{{"key",key},{"version",1},{"description",description},{"defaultAttention",{{"mode","explicit"}}},
        {"snapshotRead",false},{"valueSchema",{{"type","object"},{"properties",std::move(fields)},{"additionalProperties",false}}}};
    auto subject=value.at("reference").get<std::string>();
    if (value.contains("state")) subject+="/"+value.at("state").get<std::string>();
    Json sample{{"key",key},{"version",1},{"subject",subject},{"fact",value.at("message")},{"value",std::move(value)}};
    return {"memory",Json::array({std::move(schema)}).dump(),Json::array({std::move(sample)}).dump()};
}
}

// 将语义判断交给主 Agent，以工具契约说明长期价值、当轮用途和排除情形。
void MainMessages::register_memory_tools() {
    reply_tools_->add({"main/memory_note",app_.mailbox_host().identity,3,{"memory_note",
        "主人直接陈述可复用的长期偏好、持续约束、未来承诺、影响后续安排的经历或对这些事实的更正时，选简短稳定的主题，"
        "摘录当前原话中的依据 excerpt。通常只收录主人确认且未来仍有用的原话。涉及拒绝保存、敏感秘密、转述、猜测或寒暄时直接交谈。"
        "收录对象是当前整条真人原话，混有不宜保存的内容时跳过。仅在收到 saved 回执后确认收录。"
        "回执含 reviewDue 表示已有同主题原件，宜用 memory_search 的 originals 对照。同一事项才复盘，主题相同本身不足以合并。"
        "主人直接纠正已送达回复的语气时，用 kind=correction 和 tone，reply 逐字摘录所指回复。仅纠正当前这次用 one_time，"
        "所指明确但范围不清用 unconfirmed。语境足以界定下次适用的场合时可直接用 ongoing，写短提示 hint 和基于现有原话的 reason。"
        "项目进度用默认 condition=progress，正式验收场合用 accepted。主人私聊随口分享小烦恼、希望自然接话时用 casual_share，"
        "reason 说明所指回复和主人原话怎样支持这个用途。draft 表示已保留、待整理。"
        "新反馈明确补足范围时，先查本轮待定原件，再填 tone.prior，可省略 reply。转述、泛泛评价和表达来源不清时直接交谈。",
        R"({"type":"object","properties":{"topic":{"type":"string"},"excerpt":{"type":"string"},"kind":{"type":"string","enum":["preference","constraint","commitment","event","correction"]},"scope":{"type":"string","enum":["ongoing","one_time","unconfirmed","private"]},"tone":{"type":"object","properties":{"reply":{"type":"string"},"hint":{"type":"string"},"reason":{"type":"string"},"condition":{"type":"string","enum":["progress","accepted","casual_share"]},"prior":{"type":"string"}},"additionalProperties":false}},"required":["topic","excerpt","kind","scope"],"additionalProperties":false})"},
        true,true,{},{},1,512,true},[this](const agent::ToolInvocation& call){return consider_memory(call);});
    reply_tools_->add({"main/memory_search",app_.mailbox_host().identity,13,{"memory_search",
        "当答案需要以前的约定、偏好或个人事实，且当前对话依据不足时检索。普通常识、寒暄、当前原话足够时直接回应。"
        "topics 填一至三个主题词，可含同义词。need 按缺少的依据填 single 或 multiple，已有依据填 context_sufficient，常识填 general。"
        "count 按回答所需给出，单个事实取一条，对照时最多三条。"
        "每轮可查一次。view 默认 current，问变更过程选 history，比较具体原话选 originals，此时 topics 也可填已知原件号。查阅 memory.matches。"
        "觉得当前事情与过去几次经历有关时，可先用 themes 按主题猜测找线索，已有 memory.themes 时也可直接选号。"
        "themes 可传空 topics 查近期主题；如果需要，下一步用 originals 一次展开其中至多三个完整原件号。"
        "语气条目已有浅层提示，追问标注理由时把其完整 reference 单独填进 originals，查阅 memory.tone_original。"
        "近期摘要的长原文返回 nextOffset，可用同一个完整编号和该 offset 继续查本条下一段。"
        "主人补充语气范围时，用 tone_pending 填一个登记项目键或当前原话重复的独特短句。查阅 memory.tone_original 后再补定。"
        "current 标明当前依据，superseded 是当时的说法，needs_review 尚有更正待核对。回答时说明适用版本，追溯时引用原话和 reference。"
        "mentions 是获准原话数。previousReference 可追溯上次更正依据。无 standing 的条目是尚未整理的原话。"
        "reviewDue 表示有待比较的原话。回顾自己实际作过的选择用 episodes，topics 可填主题或投递原件号。"
        "why 查现在对过去的解释，change 查相似情境下实际换过的选择。用一个主题或当时那场的投递号查配套依据，need 用 multiple，count 给三。"
        "从 memory.experiences 区分当时感受与后来推测。查本体工具任务用 tasks，填目标词、工具号或原件号。memory.tasks 区分 goal、plan、"
        "实际派发 execution、工具 effect 与独立读回 verification。引用 actionRef、receiptRef 和已有的 readbackRef。按号展开原件，"
        "nextAction 可追到其他提议，长原文下轮用 nextOffset 续查。主人问我从过去几次任务学到了什么、这个看法还成立吗，选 understanding。"
        "取一至三个能区分对象、动作或同义措辞的主题词，也可单独填任务号或版本号。主题只查当前看法。ambiguous 时请主人说清对象。"
        "问现场现在怎样需新采样，只问一次行动查 tasks。发现反例应说明原判断的局限。"
        "claim 是我目前或当时的理解，facts 才是实际观察。区分 current、challenged、superseded，指出反例并引用行动和读回，"
        "previousReference 可追溯旧看法。nextOffset 可按证据条数翻页。子 Agent 来信用 letters，主题填待收、交付号或原信号；"
        "摘要和正文保留原作者。digest 是本体按角色选的发信人原话，pending 或 fallback 时按需展开。"
        "bodyRef、noteRef 展开已交付副本，摘录可按 offset 查看上下文。读懂待收的信后用 memory_ack 按号签收。"
        "不知道资料在哪个库时用 knowledge_catalog，按主题、来源或标签读库的概要，可传空 topics 看目录。"
        "主人问已发布的做法时用 knowledge，填条目键、主题或刚看到的单个库名，引用当前版并说明适用条件。需要原文时用 knowledge_source，"
        "topics 只填一条外部资料完整原件号。返回 nextOffset 时可沿此号查下一段。引文属于资料正文。"
        "跨工具对照用 comparisons，填一至三个主题词，先写对象或常用别称，可用度量、条件、检查点区分，也可单填比较键或版本号。"
        "多个词须同时符合，ambiguous 时询问具体对象或条件。交代对象、量和检查点，区分独立读回、来信作者与自己的解释。"
        "同检查点独立读数矛盾时保留疑问并引用两份原件。仅有报告时只归于发信人。"
        "比较两次渠道采样用 observations，topics 分别填两次任务的完整名称或读回回执号。按返回的理由说明可比性，转发须追到原件。",
        R"({"type":"object","properties":{"topics":{"type":"array","items":{"type":"string"}},"count":{"type":"integer"},"need":{"type":"string","enum":["single","multiple","context_sufficient","general"]},"view":{"type":"string","enum":["current","history","originals","themes","episodes","why","change","tasks","understanding","letters","knowledge","knowledge_catalog","knowledge_source","comparisons","observations","tone_pending"]},"offset":{"type":"integer"}},"required":["topics","count","need"],"additionalProperties":false})"},
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
    const bool tone=args.contains("tone");
    if (!tone && args.at("scope")!="ongoing")
        return memory_receipt({{"state","skipped"}},agent::ToolEffect::not_applied);
    const auto excerpt = args.at("excerpt").get<std::string>();
    if (!owner_memory_turn_ || parent_.first!=call.attempt.step.run ||
        excerpt.find_first_not_of(" \r\n\t")==excerpt.npos || excerpt.size()>2048)
        return memory_receipt({{"state","invalid_source"}},agent::ToolEffect::not_applied,false);
    const auto ref = store_->conversation()+"/"+std::to_string(parent_.second);
    Json receipt{{"state","saved"},{"reference",ref}};
    try {
        const auto kind = args.at("kind").get<std::string>();
        if (tone) {
            if (kind!="correction") throw std::invalid_argument("tone: correction required");
            const auto prior=args.at("tone").find("prior");
            if (prior!=args.at("tone").end() && (!tone_read_ || *prior!=*tone_read_ || call.attempt.step.sequence<=search_step_))
                throw std::invalid_argument("tone: original has not been read in this turn");
            const auto feedback=store_->note_tone(parent_.second,args);
            if (feedback.at("state")=="skipped") return memory_receipt(feedback,agent::ToolEffect::not_applied);
            tone_pending_=feedback.at("reference").get<std::string>();
            tone_due_|=feedback.at("state")=="draft";
            memory_revision_=store_->memory_revision();
            return memory_receipt(feedback,agent::ToolEffect::applied);
        }
        const auto result = store_->change_memory(ref,args.at("topic").get<std::string>(),excerpt,kind);
        memory_revision_ = result.revision;
        if (result.review_due) receipt["reviewDue"] = kind=="correction" ? "correction" : "compare";
    } catch (const std::invalid_argument&) {
        return memory_receipt({{"state","invalid_source"}},agent::ToolEffect::not_applied,false);
    }
    return memory_receipt(std::move(receipt),agent::ToolEffect::applied);
}

// 按实际受众和本轮额度签发检索，允许浅层引用进入同回合的深查。
std::unique_ptr<agent::ToolWork> MainMessages::search_memory(const agent::ToolInvocation& call) {
    if (!owner_memory_turn_ || tone_read_)
        return memory_receipt({{"state","unavailable"}},agent::ToolEffect::not_applied,false);
    const auto args = Json::parse(call.call.arguments);
    try {
        const auto topics = args.at("topics").get<std::vector<std::string>>();
        const auto view = args.value("view",std::string{"current"});
        const bool document=view=="knowledge_source";
        const bool exact_text=view=="originals" && topics.size()==1 && topics.front().find('/')!=std::string::npos;
        const bool page_text=exact_text && args.contains("offset");
        if (document && (topics.size()!=1 || (args.contains("offset") && args.at("offset")<0)))
            throw std::invalid_argument("knowledge: one original reference and a nonnegative offset required");
        const auto offset = args.value("offset",std::size_t{});
        std::string_view library="";
        if (search_result_) {
            const auto prior=Json::parse(search_result_->samples).at(0);
            const bool catalog_selection=view=="knowledge" && can_open_library(prior,topics);
            const bool linked=catalog_selection || (document && can_open_document(prior,topics.front(),offset)) ||
                (page_text && next_source_page(prior,"memory.matches",topics.front(),offset)) ||
                (view=="originals" && !args.contains("offset") && can_open_theme(prior,topics));
            if (!linked || call.attempt.step.sequence<=search_step_)
                return memory_receipt({{"state","unavailable"}},agent::ToolEffect::not_applied,false);
            if (catalog_selection) library=topics.front();
        }
        const auto& need = args.at("need");
        if (need=="context_sufficient" || need=="general")
            return memory_receipt({{"state","skipped"}},agent::ToolEffect::not_applied);
        const auto& count = args.at("count");
        if (count<=0) return memory_receipt({{"state","invalid_query"}},agent::ToolEffect::not_applied,false);
        const unsigned limit = need=="single" ? 1u : 3u;
        const bool own = view=="episodes" || view=="why" || view=="change";
        const auto take = count>=limit ? limit : count.get<unsigned>();
        if (args.contains("offset") && ((!document && !page_text && view!="tasks" && view!="understanding" && view!="letters" && view!="comparisons") || args.at("offset")<0))
            throw std::invalid_argument("memory: invalid offset");
        if (view=="tone_pending") {
            if (topics.size()!=1) throw std::invalid_argument("tone: one project or quoted phrase required");
            tone_read_=store_->unclear_tone(topics.front(),parent_.second);
            search_step_=call.attempt.step.sequence;
            const bool found=!tone_read_->empty();
            return memory_receipt({{"state",found ? "ready" : "no_match"},{"count",found ? 1 : 0},{"limited",false}},agent::ToolEffect::not_applied);
        }
        if (exact_text && !page_text && !store_->tone_original(topics.front()).is_null()) {
            tone_read_=topics.front();
            search_step_=call.attempt.step.sequence;
            return memory_receipt({{"state","ready"},{"count",1},{"limited",false}},agent::ToolEffect::not_applied);
        }
        auto result = document ? store_->knowledge_document_page(topics.front(),offset) :
            view=="knowledge_catalog" ? store_->search_knowledge_catalog(topics,take) :
            view=="themes" ? store_->search_memory_themes(topics,take) :
            page_text ? store_->recent_original(topics.front(),offset) :
            view=="observations" ? store_->compare_observations(topics) :
            view=="comparisons" ? store_->search_cases(topics,take,offset) :
            view=="knowledge" ? store_->search_knowledge(topics,take,0,false,nullptr,library) : view=="letters" ? store_->search_letters(topics,take,offset) :
            view=="understanding" ? store_->search_understandings(topics,take,offset) :
            view=="tasks" ? store_->search_tasks(topics,take,offset) :
            own ? store_->search_experience(topics,take,view) : store_->search_memory(topics,take,view);
        if (result.is_null()) result={{"items",Json::array()},{"limited",false}};
        if (exact_text && !page_text && result.at("items").empty()) {
            auto original=store_->recent_original(topics.front());
            if (!original.is_null()) result=std::move(original);
        }
        const auto size = result.at("items").size();
        search_result_ = document ? knowledge_document_source(topics.front()+"/"+std::to_string(offset),call.attempt.step.run.sequence,result) :
            view=="knowledge_catalog" ? knowledge_catalog_source(store_->conversation(),call.attempt.step.run.sequence,result) :
            view=="themes" ? theme_source(store_->conversation(),call.attempt.step.run.sequence,result) :
            view=="observations" ? observation_source(call.attempt.step.run.sequence,result) :
            view=="comparisons" ? case_source(call.attempt.step.run.sequence,result) :
            view=="knowledge" ? knowledge_source(store_->conversation(),call.attempt.step.run.sequence,result) :
            view=="letters" ? letter_source(call.attempt.step.run.sequence,result) :
            view=="understanding" ? understanding_source(call.attempt.step.run.sequence,result) :
            view=="tasks" ? task_source(call.attempt.step.run.sequence,result) :
            own ? experience_source(call.attempt.step.run.sequence,result) :
            matches_source(page_text ? topics.front()+"/"+std::to_string(offset) : store_->conversation(),
                call.attempt.step.run.sequence,result);
        search_step_ = call.attempt.step.sequence;
        return memory_receipt({{"state",size ? "ready" : result.value("ambiguous",false) ? "ambiguous" : result.at("limited")==true ? "limited" : "no_match"},
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
        app_.subagents().invalidate_memory(0,result.reference);
        reflection_->interrupt();
        search_result_.reset();
        tone_read_.reset();
        tone_pending_.reset();
        if (app_.conversation().busy() && owner_memory_turn_) app_.conversation().cancel();
    }
}

// 主人修改指定分身授权时，先事务写盘，再封住读过旧授权的运行与模型答复。
void MainMessages::grant_avatar(const std::string& plugin,const std::string& channel,const std::string& account,int access,std::string_view source) {
    const auto id=store_->grant_avatar(plugin,{plugins::MemoryPreset::avatar,account,channel},access,source);
    if (id) app_.subagents().invalidate_memory(id);
}

// 保存已核实的整理提议；存储暂时失败时保留待办供下次恢复。
void MainMessages::settle_tone() {
    if (!tone_due_) return;
    try { store_->settle_tones(); tone_due_=false; }
    catch (const std::runtime_error&) {
        storage_failed();
        retry_at_=std::chrono::steady_clock::now()+std::chrono::milliseconds(250);
    }
}

// 按需展开优先，其次保留当前反馈或装配一条浅层记忆，普通私聊只提供候选。
// 步骤：按号复核查证材料 → 核对待整理反馈 → 查询并投影当前语境的浅层材料。
std::optional<agent::DecisionStatusSource> MainMessages::tone_projection(std::string_view prompt,bool first_decision) const {
    if (tone_read_) {
        if (tone_read_->empty()) return {};
        auto evidence=store_->tone_original(*tone_read_);
        if (!evidence.is_null()) return tone_source("memory.tone_original","获准的纠正原话，已整理时附标注理由。",std::move(evidence),
            {"state","quote","reason","replyReference","basisReference","basisQuote","rewriteReference"});
        return {};
    }
    if (tone_pending_) {
        auto evidence=store_->tone_original(*tone_pending_,true);
        if (!evidence.is_null()) return tone_source("memory.tone_pending","本轮待整理的主人语气反馈，先用于本次回复。",std::move(evidence),
            {"state","excerpt"});
    }
    auto evidence=tone_context_current_ ? store_->tone_for(prompt,tone_recall_,first_decision) : Json(nullptr);
    if (evidence.is_null()) return {};
    if (evidence.contains("project"))
        return tone_source("memory.tone","当前汇报语境适用的语气偏好。本轮明确要求优先，按需查原文或理由。",std::move(evidence),
            {"project","condition","hint"});
    return tone_source("memory.tone_candidate","仅在本轮语境符合适用条件时采用的语气候选。本轮明确请求优先，用途不明时暂缓采用。需要原文或理由时按号查证。",
        std::move(evidence),{"condition","hint"});
}

// 将获准原话及接续任务作为带来源的数据，沿正式决策尾注和上下文预算送模。
// 步骤：按受众及修订隔离历史、投递和快照 → 固定私聊所选原件 → 在送模前逐步采样。
void MainMessages::prepare_memory(agent::RunOptions& options, DialogueAudience audience, const std::string& prompt) {
    reflection_->interrupt();
    search_result_.reset();
    tone_read_.reset();
    tone_pending_.reset();
    tone_context_current_=true;
    resume_context_current_=true;
    owner_memory_turn_ = audience==DialogueAudience::owner;
    if (owner_memory_turn_) settle_tone();
    memory_revision_ = store_->memory_revision();
    if (!memory_revision_ && !reply_tools_ && audience!=DialogueAudience::private_task && memory_binding_=="main") return;
    auto& status = *options.decision_status;
    if (memory_revision_) status.binding = std::string(owner_memory_turn_ ? "main/owner/" : "main/other/") + std::to_string(memory_revision_);
    // 任务从独立上下文起步；主人可接着追问刚退出的任务，房间和撤权后的新修订另起上下文。
    const auto task_prefix = "main/task/"+std::to_string(std::max<std::int64_t>(1,memory_revision_))+"/";
    if (audience==DialogueAudience::private_task || (owner_memory_turn_ && incoming_task_ && incoming_task_->owner_only)) {
        const auto previous = app_.conversation().status();
        status.binding = task_prefix+std::to_string(previous ? previous->turn+1 : 1);
    } else if (owner_memory_turn_ && memory_binding_.starts_with(task_prefix)) status.binding = memory_binding_;
    if (memory_binding_ != status.binding) {
        const auto previous = app_.conversation().status();
        clear_context(previous ? previous->turn : 0);
        memory_binding_ = status.binding;
    }
    constexpr std::string_view prefix = "回想 ";
    if (!owner_memory_turn_) return;
    const auto recent=prepare_recent_context(options);
    auto check=incoming_task_ && incoming_task_->check_live ? prepare_resume_check(options,incoming_task_->reference,incoming_task_->message) : nullptr;
    const bool theme_lookup=reply_tools_ && !incoming_task_ && !incoming_memory_ && !prompt.empty() && prompt.size()<=2048 &&
        !prompt.starts_with(prefix) && !memory_capture_topic(prompt) && !prompt.starts_with("撤回记忆 ");
    status.sample = [channel=weak_from_this(),other=std::move(status.sample),task=incoming_task_,prompt,recent,theme_lookup,
        check=std::move(check),first_decision=true]() mutable {
        auto sources = other ? other() : std::vector<agent::DecisionStatusSource>{};
        const auto current = channel.lock();
        if (!current) return sources;
        if (auto notice=current->inbox_notice()) sources.push_back(std::move(*notice));
        if (current->search_result_) sources.push_back(*current->search_result_);
        else if (task && current->resume_context_current_) {
            if (auto selected=current->resumed_task_source(task->reference,task->message)) sources.push_back(std::move(*selected));
        }
        if (check) if (auto reading=check()) sources.push_back(std::move(*reading));
        if (first_decision && recent)
            if (auto restored=current->restored_recent_context()) sources.push_back(std::move(*restored));
        if (first_decision && theme_lookup) {
            const auto themes=current->store_->search_memory_themes({},1,prompt);
            if (!themes.at("items").empty()) sources.push_back(theme_source(current->store_->conversation(),current->store_->watermark(),themes));
        }
        if (auto tone=current->tone_projection(prompt,std::exchange(first_decision,false))) sources.push_back(std::move(*tone));
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
