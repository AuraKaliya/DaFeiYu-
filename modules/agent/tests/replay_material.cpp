#include "support/replay_material.hpp"
#include "support/replay_evidence.hpp"
#include <iostream>

namespace {
using namespace dfy::tests;
// 报告文件材料与受控重跑范围不符。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证坏材料在解释阶段拒绝，不执行降级路径。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid replay material accepted");
}
// 拒绝未知入口、重复字段、错误类型、版本和缺少预期。
void invalid(const material::Json& original) {
    auto value = original; value["schema_version"] = 2; rejects([&] { parse_replay_material(value.dump()); });
    value = original; value["endpoint"] = "https://example.invalid"; rejects([&] { parse_replay_material(value.dump()); });
    value = original; value["fixtures"][0]["plugin_path"] = "production.dll"; rejects([&] { parse_replay_material(value.dump()); });
    value = original; value["fixtures"][0]["id"] = "production/tool"; rejects([&] { parse_replay_material(value.dump()); });
    value = original; value["expected"].erase("model_requests"); rejects([&] { parse_replay_material(value.dump()); });
    value = original; value["policy"]["tool_limit"] = 1.5; rejects([&] { parse_replay_material(value.dump()); });
    value = original; value["timeout_ms"] = -1; rejects([&] { parse_replay_material(value.dump()); });
    value = original; value["decisions"] = {"automatic"}; rejects([&] { parse_replay_material(value.dump()); });
    const auto text = original.dump(); rejects([&] { parse_replay_material("{\"schema_version\":1,"+text.substr(1)); });
    rejects([&] { parse_replay_material(std::string(2*1024*1024+1,' ')); });
}

// 用固定报告检验案例、版本及行为差异的边界。
// 步骤：分别执行同一材料 → 校验代码版本变化及未知信息 → 以受控期限验证实际回归报告。
void comparison(const material::Json& original) {
    using dfy::agent::RunVersionLabels;
    RunVersionLabels labels;
    labels.application_commit = "fixture-source-a";
    labels.build = "fixture-comparison";
    const auto before = measure_material(original.dump(),labels);
    labels.application_commit = "fixture-source-b";
    const auto after = measure_material(original.dump(2),labels);
    check(compare_replays(before,after).state == ReplayComparisonState::incomparable,"undeclared revision was comparable");
    constexpr std::string_view source_change[] = {"application_commit"};
    const auto same = compare_replays(decode_evidence(encode_evidence(before)),decode_evidence(encode_evidence(after)),source_change);
    check(same.state == ReplayComparisonState::equivalent && same.behavior.empty() && same.versions.changed.size() == 1,
        "same fixed inputs changed behavior");

    auto revised = after;
    auto& trace = revised.report.diagnostic.timeline;
    ++trace.goal_revision;
    trace.model_contexts.back().goal_revision = trace.goal_revision;
    const auto context_change = compare_replays(before,decode_evidence(encode_evidence(revised)),source_change);
    check(context_change.state == ReplayComparisonState::differs &&
        context_change.behavior == std::vector<std::string_view>{"goal_revision","model_contexts"},
        "unchanged model nodes hid a change of goal context");

    auto observed_left = before; auto observed_right = after;
    auto& left_samples = observed_left.report.diagnostic.timeline.observations;
    auto& right_samples = observed_right.report.diagnostic.timeline.observations;
    left_samples = {{1,2,{"first",1},1},{1,2,{"second",1},1}};
    right_samples = {{5,6,{"first",1},1},{5,6,{"second",1},1}};
    check(compare_replays(observed_left,observed_right,source_change).state == ReplayComparisonState::equivalent,
        "independent observation IDs changed behavior");
    right_samples[1].source = 7;
    const auto source_split = compare_replays(observed_left,decode_evidence(encode_evidence(observed_right)),source_change);
    check(source_split.state == ReplayComparisonState::differs && source_split.behavior == std::vector<std::string_view>{"observations"},
        "observation source lineage changed without evidence");

    labels.application_commit.reset();
    const auto unknown = measure_material(original.dump(),labels);
    check(compare_replays(before,unknown,source_change).reason == "required versions are unknown","unknown versions compared as equal");
    auto changed = original;
    changed["version"] = 2;
    check(compare_replays(before,measure_material(changed.dump(),labels)).reason == "case inputs differ","different cases were comparable");

    auto deadline = material::parse(R"({
      "schema_version":1,"id":"bounded-time","version":1,"prompt":"wait","timeout_ms":200,
      "policy":{"revision":1,"model_limit":4,"tool_limit":4,"allow_observation":false,"confirm_changes":false},
      "fixtures":[],"steps":[{"last_role":"user","tools":[],"advance_ms":50,"response":{"text":"answer","finish":"stop"}}],
      "decisions":[],"expected":{"model_requests":1,"tool_calls":0,"history_messages":2,"result":"succeeded","stop_reason":"none","effects":[]}
    })");
    labels.application_commit = "fixture-source-a";
    const auto allowed = measure_material(deadline.dump(),labels);
    deadline["timeout_ms"] = 10;
    const auto timed_out = measure_material(deadline.dump(),labels);
    constexpr std::string_view policy_change[] = {"execution_policy"};
    const auto changed_behavior = compare_replays(decode_evidence(encode_evidence(allowed)),decode_evidence(encode_evidence(timed_out)),policy_change);
    check(changed_behavior.state == ReplayComparisonState::differs &&
        std::find(changed_behavior.behavior.begin(),changed_behavior.behavior.end(),"stop_reason") != changed_behavior.behavior.end() &&
        std::find(changed_behavior.behavior.begin(),changed_behavior.behavior.end(),"fact_order") != changed_behavior.behavior.end() &&
        !timed_out.report.failed_assertions.empty(),"actual deadline regression was hidden");
    auto damaged = material::parse(encode_evidence(before));
    damaged["model_calls"] = 99;
    rejects([&] { decode_evidence(damaged.dump()); });
    damaged = material::parse(encode_evidence(before));
    damaged["failed_assertions"] = {"unknown assertion"};
    rejects([&] { decode_evidence(damaged.dump()); });
    damaged = material::parse(encode_evidence(before));
    damaged["restore"] = true;
    rejects([&] { decode_evidence(damaged.dump()); });
    std::cout << "Compared " << timed_out.report.model.case_id << " v" << timed_out.report.model.case_version << '\n';
    for (const auto name : changed_behavior.behavior) std::cout << "Behavior changed " << name << '\n';
}

// 改变同一尝试中确认所指的候选，其他结算记录保持可读且不变。
// 步骤：运行两个工具的固定批次 → 换绑另一侧确认关联 → 验证比较识别结构变化。
void confirmation_binding(material::Json original) {
    auto second = original.at("fixtures").at(0);
    second["id"] = "fixture/read"; second["name"] = "read"; second["changes_state"] = false;
    second["results"][0]["effect"] = "not_applied";
    original["fixtures"].push_back(std::move(second));
    for (auto& step : original["steps"]) step["tools"].push_back("read");
    original["steps"][0]["response"]["tool_calls"].push_back({{"id","two"},{"name","read"},{"arguments",material::Json::object()}});
    original["steps"][1]["last_tool_id"] = "two";
    original["expected"]["tool_calls"] = 2; original["expected"]["history_messages"] = 5;
    original["expected"]["effects"].push_back("not_applied");
    const auto material = original.dump();
    dfy::agent::RunVersionLabels labels; labels.application_commit = "fixture-source"; labels.build = "fixture-comparison";
    const auto baseline = measure_material(material,labels); auto changed = measure_material(material,labels);
    auto& trace = changed.report.diagnostic.timeline;
    using dfy::agent::TraceNodeKind;
    const auto control = std::find_if(trace.nodes.begin(),trace.nodes.end(),[](const auto& node) { return node.id.kind == TraceNodeKind::control; });
    const auto read = std::find_if(trace.nodes.begin(),trace.nodes.end(),[](const auto& node) { return node.tool_id == "fixture/read"; });
    check(baseline.report.failed_assertions.empty() && changed.report.failed_assertions.empty() &&
        control != trace.nodes.end() && read != trace.nodes.end() && control->related != read->id.local,"invalid comparison fixture");
    control->related = read->id.local;
    for (auto& fact : trace.delta.facts) if (fact.subject == control->id) fact.related = read->id.local;
    const auto comparison = compare_replays(baseline,decode_evidence(encode_evidence(changed)));
    check(comparison.state == ReplayComparisonState::differs && comparison.behavior == std::vector<std::string_view>{"nodes"},
        "comparison hid a confirmation bound to a different candidate");
}
}
// 读取固定 JSON 夹具后重跑两次，文件无法指定真实模型或任意执行函数。
// 步骤：读取明确夹具 → 两次重跑核对行为 → 验证错误材料拒绝 → 报告结果。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2,"expected fixed material path"); const auto scenario = load_replay_material(argv[1]);
        const auto first = replay_scenario(scenario), second = replay_scenario(scenario);
        check(first.tool_calls == 1 && second.tool_calls == 1 && first.effects == second.effects && first.model.consumed == 2,
            "file replay changed deterministic facts");
        auto changed_expectation = scenario;
        ++changed_expectation.expected.tool_calls;
        const auto failed = measure_replay(changed_expectation);
        check(failed.tool_calls == first.tool_calls && failed.effects == first.effects &&
            failed.failed_assertions == std::vector<std::string_view>{"tool_calls"} &&
            dfy::agent::review_timeline(failed.diagnostic.timeline).run_exited,"wrong expectation erased actual replay evidence");
        rejects([&] { replay_scenario(changed_expectation); });
        std::ifstream file(std::filesystem::path(argv[1]),std::ios::binary); material::Json original; file >> original;
        invalid(original);
        comparison(original);
        confirmation_binding(original);
        std::cout << "Replay material checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
