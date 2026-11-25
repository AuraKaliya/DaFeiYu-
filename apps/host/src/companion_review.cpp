#include "companion.hpp"
#include <nlohmann/json.hpp>

namespace dfy::app {
namespace {
// 从可信配对读回提取三次真实返回及覆盖计数。
struct SurveySample {
    std::uint64_t window, sample, seen, gained, last;
    nlohmann::json trips;
    std::string area;
};

// 拒绝异任务、缺字段、错序或自相矛盾的观察。
// 步骤：检查原任务和结构 → 核对序号与覆盖水位 → 复制有界事实。
std::optional<SurveySample> read_sample(const nlohmann::json& data, std::uint64_t order) {
    const auto& fact = data.at("survey");
    if (!fact.is_object() || !fact.contains("order") || fact["order"] != order ||
        !fact.contains("returns") || !fact["returns"].is_array() || fact["returns"].size() != 3 ||
        !fact.contains("area") || !fact["area"].is_string()) return {};
    for (auto name : {"window", "sample", "seen", "gained"})
        if (!fact.contains(name) || !fact[name].is_number_unsigned()) return {};
    const auto window = fact["window"].get<std::uint64_t>(), sample = fact["sample"].get<std::uint64_t>();
    const auto seen = fact["seen"].get<std::uint64_t>(), gained = fact["gained"].get<std::uint64_t>();
    const auto& trips = fact["returns"];
    std::uint64_t last = 0;
    for (const auto& point : trips) {
        if (!point.is_number_unsigned() || point <= last || point > sample) return {};
        last = point.get<std::uint64_t>();
    }
    const auto& area = fact["area"].get_ref<const std::string&>();
    if (!window || gained > seen || seen > 256 || area.empty() || area.size() > 48 ||
        area.find_first_not_of("0123456789-,") != area.npos) return {};
    return SurveySample{window, sample, seen, gained, last, trips, area};
}

// 把只读路线答复关联到原询问，未返回原身份时登记未核实。
// 步骤：比对读请求身份 → 核验原问题和答复 → 记录消息因果。
void receive_answer(ExplorationReview& review, CompanionMessages& messages, const CompanionAddress& address,
    const CompanionVisit& visit, std::uint64_t request, const nlohmann::json& data) {
    if (review.address == address && review.task == visit.task &&
        review.answer_request == request && !review.answer) {
        std::string reply;
        if (data.is_object() && data.contains("answer")) {
            const auto& a = data["answer"];
            if (a.is_object() && a.contains("order") && a["order"] == visit.order && a.contains("inquiry") &&
                a["inquiry"] == std::to_string(review.inquiry) && a.contains("text") && a["text"].is_string())
                reply = a["text"].get<std::string>();
        }
        const bool valid = !reply.empty() && reply.size() <= 512;
        auto delivered = messages.post(address, CompanionSide::game, valid ? CompanionMessageKind::answer : CompanionMessageKind::failed,
            messages.next(CompanionSide::game), review.inquiry, valid ? reply : "游戏侧未确认本次路线询问");
        review.answer = delivered.revision;
        review.reply = std::move(delivered.text);
    }
}
}

// 只对原世界、原查看任务的一份新鲜读回做一次判断。重复采样和子会话的文字都不能证明恢复。
// 步骤：核对来源与样本 → 区分新增覆盖和重复返回 → 重读后向子会话追问 → 用后续观察验证进展。
void CompanionSession::review_exploration() {
    const auto& state = reader_.state();
    if (!survey_tool_ || !address_ || !visit_ || visit_->address != *address_ || messages_.task() != visit_->task ||
        state.pending || state.request == reviewed_request_ || state.link != CompanionLink::available || !state.last) return;
    reviewed_request_ = state.request;
    const auto data = nlohmann::json::parse(state.last->payload, nullptr, false);
    if (review_) receive_answer(*review_, messages_, *address_, *visit_, state.request, data);
    if (!data.is_object() || !data.contains("survey") || !data.contains("signalAgeMs") ||
        !data["signalAgeMs"].is_number_unsigned() || data["signalAgeMs"] > 3000) {
        if (review_ && review_->stage == ReviewStage::rechecking) {
            review_->stage = ReviewStage::interrupted;
            review_->verification = "缺少连续的新鲜观察，尚不能判断是否受阻";
        }
        return;
    }
    const auto report = read_sample(data, visit_->order);
    if (!report) return;
    const auto& [window, sample, seen, gained, last, trips, area] = *report;
    const bool original = review_ && review_->address == *address_ && review_->task == visit_->task;
    if (original && (window < review_->window || sample <= review_->sample)) return;
    const bool same = original && review_->window == window;
    if (original && !same && review_->inquiry && !review_->recovery) {
        review_->stage = ReviewStage::interrupted;
        review_->verification = "观察窗口已更换，新窗口的覆盖数不能核实上次询问后的恢复";
        return;
    }
    if (same && (seen < review_->coverage || last < review_->last_return)) return;

    if (same && review_->inquiry && !review_->recovery) {
        if (seen > review_->coverage) {
            review_->verification = "新样本 " + std::to_string(sample) + " 确认已观察区域从 " +
                std::to_string(review_->coverage) + " 增至 " + std::to_string(seen) + "，探索已恢复";
            review_->recovery = messages_.post(*address_, CompanionSide::game, CompanionMessageKind::report,
                messages_.next(CompanionSide::game), visit_->task, review_->verification).revision;
            review_->recovery_sample = sample;
            review_->stage = ReviewStage::recovered;
        }
        review_->sample = sample;
        return;
    }
    if (same && review_->recovery && (trips.front() <= review_->recovery_sample || !review_->answer)) return;
    const auto describe = "区域 " + area + " 的三次返回采样 " + trips.dump() + "，新增观察区域 " + std::to_string(gained);
    if (visit_->phase != VisitPhase::submitted) return;
    const bool confirmed = same && !gained && review_->stage == ReviewStage::rechecking &&
        review_->area == area && review_->coverage == seen;
    if (!confirmed) {
        if (same && review_->stage != ReviewStage::interrupted && review_->area == area && review_->last_return == last && review_->coverage == seen) return;
        ExplorationReview result{};
        result.address = *address_; result.task = visit_->task; result.order = visit_->order;
        result.window = window; result.sample = sample; result.coverage = seen; result.last_return = last; result.area = area;
        result.basis = describe + (gained ? "，继续探索" : "，先核实当前进展");
        result.report = messages_.post(*address_, CompanionSide::game, CompanionMessageKind::report,
            messages_.next(CompanionSide::game), result.task, result.basis).revision;
        if (!gained) { result.stage = ReviewStage::rechecking; result.verification = "等候原世界的新采样"; }
        review_ = std::move(result);
        if (!gained) refresh();
        return;
    }
    if (queued_ || messages_.awaiting_answer()) return;
    const auto prompt = describe + "。现在要去哪里？寻路器最近返回了什么？";
    review_->inquiry = messages_.post(*address_, CompanionSide::primary, CompanionMessageKind::inquiry,
        messages_.next(CompanionSide::primary), review_->report, prompt).revision;
    try {
        const auto args = nlohmann::json{{"inquiry", std::to_string(review_->inquiry)}, {"order", visit_->order}}.dump();
        review_->answer_request = reader_.refresh(catalog_->snapshot(std::span(&*survey_tool_, 1), false), clock_, args);
    } catch (const std::exception&) {
        const auto failed = messages_.post(*address_, CompanionSide::game, CompanionMessageKind::failed,
            messages_.next(CompanionSide::game), review_->inquiry, "未能提交路线查询");
        review_->answer = failed.revision; review_->reply = failed.text;
    }
    review_->sample = sample;
    review_->stage = ReviewStage::asked;
    review_->verification = "已向游戏中的她询问，等待后续观察验证";
}
}
