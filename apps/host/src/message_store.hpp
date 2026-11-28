#pragma once
#include "comparison_tag.hpp"
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace dfy::plugins { struct MemoryIdentity; }

namespace dfy::app {
// 识别主聊天显式收录格式，返回原句中由主人指定的主题。
std::optional<std::string_view> memory_capture_topic(std::string_view text);
struct StoredMessage {
    std::int64_t id, revision, utc_ms;
    std::string source, submission;
    nlohmann::json envelope, receipt;
};
struct MessageAdmission { StoredMessage message; bool created; };
struct MessagePage { std::vector<StoredMessage> records; bool more = false; };
struct MemoryChange { std::int64_t revision; bool changed, review_due; std::string reference; };
struct ChildBinding { std::int64_t owner; std::string shared_source; };
enum class IdleQueue : unsigned {cross_tool,tool_family,letter_digest};
// 固定三类本体空闲工作的轮转次序，空队列可在同一轮跳过。
constexpr IdleQueue following_queue(IdleQueue queue) {return static_cast<IdleQueue>((static_cast<unsigned>(queue)+1)%3);}

// 每个数据目录拥有一个跨启动的主会话，原文与有序投递回执在同一库内提交。
class MessageStore {
  public:
    // 打开系统 SQLite，事务内初始化首次使用的数据结构与逻辑会话号。
    explicit MessageStore(const std::filesystem::path& profile);
    // 在最后一个使用者释放后关闭数据库连接。
    ~MessageStore();
    // 返回已写入此目录且跨启动稳定的逻辑会话标识。
    const std::string& conversation() const noexcept;
    // 按来源和稳定提交身份读取原文与最新回执。
    std::optional<StoredMessage> find(std::string_view source, std::string_view submission) const;
    // 原命令轮询只读取小份最新回执，不提取消息正文。
    nlohmann::json receipt(std::int64_t message) const;
    // 原子提交原文与首份回执，同号同参返回原结果，同号异参报告冲突。
    MessageAdmission put(std::string_view source, std::string_view submission,
        const nlohmann::json& envelope, const nlohmann::json& receipt);
    // 仅在回执变化时追加提交序号，再原子发布为该消息的最新结果。
    void update(std::int64_t message, const nlohmann::json& receipt);
    // 读取最后一份已提交回执的序号。
    std::int64_t watermark() const;
    // 在固定水位向前补读回执，或以消息号倒序读取当时的历史投影。
    MessagePage page(bool history, std::int64_t cursor, std::int64_t through, unsigned limit) const;
    // 为受信主聊天读取最近获准的任务卡和可用于接续的完整原件号。
    nlohmann::json recent_tasks() const;
    // 按消息号读取一批尚需核对的投递，已结算历史不进入启动扫描。
    std::vector<StoredMessage> unfinished(std::int64_t after) const;
  private:
    friend class MainMessages;
    friend class TaskReflection;
    friend class Subagents;
    // 跨启动恢复最近一次已完成工作的后继类别。
    IdleQueue next_idle_queue() const;
    // 给管理入口提供可跨启动核对的档案身份与版本，不读取私人内容。
    nlohmann::json profile_info() const;
    // 将真实包身份与结构化预设绑定到跨启动的子库。
    ChildBinding bind_child(std::string_view plugin,const plugins::MemoryIdentity& identity);
    // 保存原子库中的一份显式记录，拒绝越权、重试冲突及失效来源。
    std::int64_t child_note(std::int64_t child,const std::string& event,const std::string& topic,const std::string& content);
    // 根据当前读权与实际受众生成限量投影，复核授权原话。
    nlohmann::json child_projection(std::int64_t child,std::string_view topic,bool outward) const;
    // 主人配置指定插件、渠道和账号的分身权限及一份明确共享的原件。
    std::int64_t grant_avatar(std::string_view plugin,const plugins::MemoryIdentity& identity,int access,std::string_view reference);
    // 入事务寄出带有稳定委托号的子库启动或结束信，重复信件沿用原号。
    std::int64_t post_letter(std::int64_t child,const std::string& exchange,unsigned mode,const std::string& persona,
        bool start,std::string_view outcome,const std::string& summary,const std::string& body,std::uint64_t note);
    // 取得尚未由宿主实际收件的最早信号。
    std::int64_t pending_letter() const;
    // 判定已恢复的交付是否已有结束信，阻止重启后重派动作。
    bool letter_finished(std::int64_t child,const std::string& exchange) const;
    // 给原地址已接收的持久信件登记收件回执。
    void receive_letter(std::int64_t id);
    // 私聊只公布有待确认的已收信件，不提前披露内容。
    bool unread_letter() const;
    // 查询已送至本体的信件快照及接收方获准的长文分段。
    nlohmann::json search_letters(const std::vector<std::string>& topics,unsigned count,std::size_t offset) const;
    // 一次确认本轮实际交给主 Agent 并获其签收的原信号。
    void acknowledge_letters(const std::vector<std::string>& references);
    // 保存有出处的候选版本；它仅供主人审阅。
    nlohmann::json propose_knowledge(const nlohmann::json& draft,std::string_view proposer);
    // 在同一事务校验当前版本及全部原件后发布或撤下，变更只由管理入口发起。
    bool publish_knowledge(const std::string& article,const std::string& edition,std::optional<std::int64_t> withdraw);
    // 主人按指定子库授予某一已发布版本的读取或转述权。
    std::int64_t grant_knowledge(const std::string& article,const std::string& edition,const std::string& plugin,
        const plugins::MemoryIdentity& identity,int rights);
    // 管理入口读取版本清单或可审阅的具体草案。
    nlohmann::json inspect_knowledge(const std::string& article,const std::string& edition) const;
    // 核实当前发布状态、源权限及目标受众后按主题投影，内部保留撤权依赖。
    nlohmann::json search_knowledge(const std::vector<std::string>& topics,unsigned count,std::int64_t child=0,bool outward=false,
        std::vector<std::string>* dependencies=nullptr) const;
    // 登记不可变的比较对象、观测量、单位、适用条件及比较口径。
    void define_case(const std::string& name,const nlohmann::json& scope);
    // 为比较维护主人认可的检索别名，原范围与出处保持稳定。
    void case_alias(const std::string& name,const std::string& alias,bool remove);
    // 在接纳带标记的工具任务前确认比较定义及检查点。
    void validate_case(const ComparisonTag& tag) const;
    // 主人事后指定某份已有回执或来信的比较条件。
    void attach_case(const ComparisonTag& tag,const std::string& reference);
    // 取得有两条独立工具路径的下一份空闲比较材料。
    std::optional<nlohmann::json> next_case() const;
    // 将本体的解释与全组状态、独立路径和反证重新核验后写入新版本。
    void commit_case(const nlohmann::json& batch,const nlohmann::json& proposal);
    // 取一封已实际收到、原件获准且明确要求私下整理的长信。
    std::optional<nlohmann::json> next_digest() const;
    // 按收件副本重验摘录位置，保存整理结果或保留原信供回退。
    void commit_digest(const nlohmann::json& batch,const nlohmann::json& proposal);
    // 查当前或某一历史版本的跨渠道证据、分歧及本体理解。
    nlohmann::json search_cases(const std::vector<std::string>& topics,unsigned count,std::size_t offset) const;
    // 用两份仍可读取的渠道回执核对同物、同次采样、来源独立性与原值。
    nlohmann::json compare_observations(const std::vector<std::string>& topics) const;
    // 取得下一份有独立原件的跨任务比较，只在提交后移动进度。
    std::optional<nlohmann::json> next_comparison() const;
    // 在同一事务重查原件、版本及判断的适用范围，通过后追加本体解释。
    void commit_understanding(const nlohmann::json& comparison, const nlohmann::json& proposal);
    // 按工具、任务主题或原版本号读取解释及分页的亲历事实。
    nlohmann::json search_understandings(const std::vector<std::string>& topics, unsigned count, std::size_t offset) const;
    // 按原运行身份一次性提交任务事实及待复盘原因。
    void save_task(const nlohmann::json& task);
    // 原子取一份仍获准的待办，以原回执整理复盘结论。
    bool review_idle_task();
    // 以主题或稳定原件号查回任务、行动或回执，长原文按字节分段。
    nlohmann::json search_tasks(const std::vector<std::string>& topics, unsigned count, std::size_t offset) const;
    // 返回当前档案的记忆授权修订，零表示从未收录原话。
    std::int64_t memory_revision() const;
    // 在一个事务内按原件身份收录或撤权，重复操作保留修订号。
    MemoryChange change_memory(std::string_view identity, const std::optional<std::string>& topic,
        std::string_view excerpt = {}, std::string_view kind = {});
    // 把两份获准原件按重复表达或明确更正建立有来源的修订关系。
    void reflect_memory(const nlohmann::json& comparison);
    // 按主题或稳定投递号读取本体亲历，复盘结论只随完整、仍获准的依据交付。
    nlohmann::json search_experience(const std::vector<std::string>& topics, unsigned count, std::string_view view) const;
    // 核实不同场次的亲历并分别追加解释修订或自主选择变化。
    void review_experience(const nlohmann::json& review);
    // 查询当前档案中最近一次实际接纳的用户原话身份。
    std::string last_memory_source() const;
    // 验证所指答复后保留语气反馈或补证，准备好的范围等待业务整理。
    nlohmann::json note_tone(std::int64_t message,const nlohmann::json& proposal);
    // 原子发布准备完成的范围与依据，关联实际送达的改写。
    void settle_tones();
    // 按项目和汇报条件查适用纠正，或按聊天用途查一条普通私聊候选。
    nlohmann::json tone_for(std::string_view prompt) const;
    // 按登记项目找最近一条可补证的未定反馈。
    std::string unclear_tone(std::string_view project) const;
    // 按原件号重验私有索引，读取原文和保存的标注理由。
    nlohmann::json tone_original(std::string_view reference,bool pending=false) const;
    // 仅由主人私聊的读取路径按主题或原身份取一份仍获准的原件。
    std::optional<StoredMessage> recall_memory(std::string_view selector) const;
    // 按本档案获准主题选择候选，在整份证据数组的字节预算内保留原件。
    nlohmann::json search_memory(const std::vector<std::string>& topics, unsigned count, std::string_view view) const;
    struct State;
    std::unique_ptr<State> state_;
};
}
