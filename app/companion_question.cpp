#include "companion_question.hpp"
#include <nlohmann/json.hpp>

namespace dfy::app {
// 仅在原查看已观察到目标后签发一次问题，发送请求与查看委托保持独立。
// 步骤：核对发现与已有问题 → 固定原来源和新问题号 → 提交唯一发送调用。
void CompanionQuestions::open(const CompanionVisit& visit, agent::ToolSnapshot scope, const agent::ToolClock& clock) {
    if (visit.phase != VisitPhase::observed || !visit.run || !visit.task || !visit.order) throw std::invalid_argument("question: missing observation");
    if (question_ && question_->address == visit.address && question_->task == visit.task) return;
    if (executor_ || (question_ && question_->phase == QuestionPhase::waiting)) throw std::logic_error("question: outstanding");
    const auto definitions = scope.definitions();
    if (definitions.size() != 1 || !scope.permits_state_changes()) throw std::invalid_argument("question: incorrect capability");
    CompanionQuestion question{visit.address, runtime::next_domain(), 1, visit.task, visit.order, 0, visit.owner, {}, {}, {}};
    const agent::RunId run{question.id, 1};
    auto executor = std::make_unique<agent::ToolExecutor>(run, std::move(scope), agent::ToolLimits{1, std::chrono::seconds(5)}, nullptr, clock);
    const auto args = nlohmann::json{{"generation", visit.address.world}, {"order", visit.order}, {"question", std::to_string(question.id)}}.dump();
    auto work = executor->submit({{run, 1}, 1}, {"ask", definitions.front().name, args});
    question_ = std::move(question);
    executor_ = std::move(executor);
    work_ = std::move(work);
}

// 从原工具回执登记提问已发出；丢失回执只报告不可核实。
// 步骤：等待实际退出 → 核对发送者绑定和原问题 → 更新问题修订并释放工作。
void CompanionQuestions::advance() {
    if (!executor_) return;
    executor_->advance();
    if (work_->exit()->state() == runtime::OperationState::pending) return;
    auto& question = *question_;
    if (question.phase == QuestionPhase::sending) {
        question.phase = QuestionPhase::unavailable;
        const auto& result = work_->outcome();
        if (result.succeeded && work_->fault() == agent::ToolError::none && result.output.size() <= 16384) {
            try {
                const auto value = nlohmann::json::parse(result.output);
                if (value.at("id") == std::to_string(question.id) && value.at("generation") == question.address.world &&
                    value.at("order") == question.order && value.at("owner") == question.owner && value.at("sent") == true &&
                    value.at("after").is_number_unsigned() && value.at("after") >= question.order) {
                    const auto text = value.at("text").get<std::string>();
                    if (!text.empty() && text.size() <= 1024) {
                        question.text = text;
                        question.after = value.at("after").get<std::uint64_t>();
                        if (value.contains("material") && value.contains("returnPlace") && value.at("returnPlace").is_string()) {
                            question.material = value.at("material").get<std::string>();
                            question.return_place = value.at("returnPlace").get<std::string>();
                        }
                        question.phase = QuestionPhase::waiting;
                    }
                }
            } catch (const std::exception&) { question.phase = QuestionPhase::unavailable; }
        }
        ++question.revision;
    }
    work_.reset();
    executor_.reset();
}

// 仅采用问出以后由原玩家对同一个问题作出的拒绝。
bool CompanionQuestions::decline(const CompanionAddress& address, std::string_view owner, std::uint64_t sequence, std::string_view id) {
    if (!question_ || question_->phase != QuestionPhase::waiting || address != question_->address ||
        question_->owner != owner || sequence <= question_->after || std::to_string(question_->id) != id) return false;
    question_->phase = QuestionPhase::declined;
    ++question_->revision;
    return true;
}

// 合并两个可信入口对同一问题的答复，只签发一次新委托。
// 步骤：验证原身份与参数 → 检查已有决定及冲突 → 预备完整回执后一次发布。
CollectionGrant CompanionQuestions::authorize(const CompanionAddress& address, const std::string& owner, std::uint64_t id,
    std::uint64_t revision, AnswerEntry entry, std::uint64_t command, std::uint64_t quantity, const std::string& answer) {
    if (!question_ || question_->address != address || question_->owner != owner || question_->id != id ||
        !command || !revision || (entry != AnswerEntry::game && entry != AnswerEntry::management) ||
        !quantity || quantity > 64 || answer.empty() || answer.size() > 1024 ||
        (entry == AnswerEntry::game && command <= question_->after)) throw std::invalid_argument("companion: invalid player decision");
    if (grant_ && grant_->question == id) {
        if (grant_->question_revision != revision || grant_->quantity != quantity ||
            (grant_->entry == entry && grant_->command == command && grant_->answer != answer))
            throw std::invalid_argument("companion: conflicting player decision");
        return *grant_;
    }
    if (question_->phase != QuestionPhase::waiting || question_->revision != revision ||
        question_->material.empty() || question_->return_place.empty()) throw std::invalid_argument("companion: unavailable collection scope");
    CollectionGrant next{address, runtime::next_domain(), id, revision, question_->task, question_->order,
        command, quantity, entry, owner, question_->material, question_->return_place, answer};
    auto receipt = next;
    grant_ = std::move(next);
    question_->phase = QuestionPhase::authorized;
    ++question_->revision;
    return receipt;
}

// 保留已解决问题的结论，未决问题在原绑定失效时关闭。
void CompanionQuestions::withdraw() {
    if (executor_) executor_->close();
    if (question_ && (question_->phase == QuestionPhase::sending || question_->phase == QuestionPhase::waiting)) {
        question_->phase = QuestionPhase::withdrawn;
        ++question_->revision;
    }
}
}
