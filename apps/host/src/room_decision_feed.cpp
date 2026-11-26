#include "room_decision_feed.hpp"
#include "builtin_room.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>

namespace dfy::app {
// 只绑定当前实例及身体，从角色已经接纳的感知中取一批历史证据。
// 步骤：固定角色身份 → 每步读取原游标之后的记录 → 按实际送入的最后一条签发新游标。
agent::DecisionObservationFeed room_decision_feed(const BuiltinRoom& room) {
    const auto binding = room.layout().room;
    const auto body = room.self(binding)->body;
    // 将最多四条角色原记录写入本次运行的观察封套，有限窗口丢过的部分显式标明。
    // 步骤：按绑定取有界页 → 编码各记录的时间、范围与原字段 → 返回待分派水位。
    const auto read = [&room,binding,body](std::uint64_t cursor, agent::AttemptId attempt, agent::ToolTime now)
        -> std::optional<agent::DecisionObservationBatch> {
        using Json = nlohmann::json;
        const auto page = room.percepts_since(binding,body,cursor);
        if (!page || page->records.empty()) return {};
        const auto count = std::min<std::size_t>(4,page->records.size());
        const auto first = page->cursor-page->records.size()+1;
        auto records = Json::array();
        for (std::size_t i=0;i<count;++i) {
            auto record = Json::parse(agent::historical_observation_input(*page->records[i],now));
            const bool device = page->records[i]->metadata().object.id=="room.lamp_monitor";
            record["origin"] = device ? "device_report" : "personal_perception";
            if (device) record["reporter"] = "desk_lamp_monitor";
            record["cursor"] = first+i;
            records.push_back(std::move(record));
        }
        const auto last = first+count-1;
        Json envelope{{"schema","dafeiyu.room_perceptions/2"},{"origin","bound_room"},
            {"session",attempt.step.run.session},{"turn",attempt.step.run.sequence},{"step",attempt.step.sequence},
            {"room",binding.instance},{"generation",binding.generation},{"body",body},{"after",cursor},{"through",last},
            {"historyGap",page->history_gap},{"records",std::move(records)}};
        return agent::DecisionObservationBatch{last,envelope.dump()};
    };
    return {std::make_shared<std::uint64_t>(0),read};
}
}
