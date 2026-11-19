#pragma once
#include "companion_mining.hpp"

namespace dfy::app {
enum class ReviewStage { exploring, rechecking, asked, recovered, interrupted };
struct ExplorationReview {
    CompanionAddress address;
    ReviewStage stage = ReviewStage::exploring;
    std::uint64_t task = 0, order = 0, window = 0, sample = 0, coverage = 0, last_return = 0;
    std::uint64_t report = 0, inquiry = 0, answer_request = 0, answer = 0, recovery = 0, recovery_sample = 0;
    std::string area, basis, reply, verification;
};

struct BoardCollection {
    CollectionGrant grant;
    std::optional<DeliveryStatus> delivery;
    std::optional<MiningStatus> mining;
};

// 一次查看及其派生委托的可信值副本，保留原始身份、修订和证据。
struct BoardTask {
    CompanionVisit visit;
    std::optional<ExplorationReview> review;
    std::optional<CompanionQuestion> question;
    std::optional<BoardCollection> collection;
    std::optional<bool> player_input_gap;
};

struct CompanionBoard {
    runtime::DomainId session = 0;
    std::vector<std::uint64_t> tasks;
    std::uint64_t discarded_through = 0;
    bool historical = false, binding_current = false;
    std::optional<BoardTask> selected;
    CompanionDelta messages;
};
}
