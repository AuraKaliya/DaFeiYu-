#include "support/replay_evidence.hpp"
#include <iostream>

namespace {
using namespace dfy::tests;

// 只读打开明确指定的本地材料或证据文件。
// 步骤：打开文件 → 有界读取 → 核对输入状态和长度。
std::string load(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);
    if (!file) throw std::runtime_error("evaluation: cannot open file");
    std::string value(evidence_limit+1,'\0');
    file.read(value.data(),static_cast<std::streamsize>(value.size()));
    if (file.bad() || file.gcount() > static_cast<std::streamsize>(evidence_limit))
        throw std::runtime_error("evaluation: file read failed or file too large");
    value.resize(static_cast<std::size_t>(file.gcount()));
    return value;
}

// 运行明确的固定替身材料，将证据写到标准输出供调用者保存。
// 步骤：读取材料和编译标签 → 执行离线场景 → 输出证据并按原断言返回状态。
int capture(const std::filesystem::path& path) {
    dfy::agent::RunVersionLabels labels;
    if (!std::string_view(DFY_REPLAY_COMMIT).empty()) labels.application_commit = DFY_REPLAY_COMMIT;
    labels.build = DFY_REPLAY_BUILD;
    const auto evidence = measure_material(load(path),std::move(labels));
    std::cout << encode_evidence(evidence) << '\n';
    return !std::cout || !evidence.report.failed_assertions.empty() ? 1 : 0;
}

// 打印比较双方的独立版本和原断言状态。
void write_source(std::string_view side, const ReplayEvidence& evidence) {
    const auto& versions = evidence.report.trace.versions;
    std::cout << side << " commit=" << versions.application_commit.value_or("unknown")
              << " build=" << versions.build.value_or("unknown") << '\n';
    for (const auto assertion : evidence.report.failed_assertions) std::cout << side << " failed assertion " << assertion << '\n';
}

// 读取两个已保存的独立执行报告并输出差异，无模型或工具执行。
// 步骤：严格解析双方证据 → 校对案例和已声明变化 → 显示比较结果及实际断言。
int compare(const std::filesystem::path& before, const std::filesystem::path& after, std::span<const std::string_view> declared) {
    const auto baseline = decode_evidence(load(before));
    const auto candidate = decode_evidence(load(after));
    const auto result = compare_replays(baseline,candidate,declared);
    std::cout << "Baseline case " << baseline.report.model.case_id << " v" << baseline.report.model.case_version
              << " candidate case " << candidate.report.model.case_id << " v" << candidate.report.model.case_version << '\n';
    write_source("Baseline",baseline);
    write_source("Candidate",candidate);
    for (const auto& version : result.versions.changed) std::cout << "Version changed " << version << '\n';
    for (const auto& version : result.versions.unknown) std::cout << "Version unknown " << version << '\n';
    if (result.state == ReplayComparisonState::incomparable) {
        std::cout << "Incomparable " << result.reason << '\n';
        return 2;
    }
    for (const auto field : result.behavior) std::cout << "Behavior changed " << field << '\n';
    if (result.state == ReplayComparisonState::equivalent) std::cout << "Equivalent behavior\n";
    return !std::cout || result.state == ReplayComparisonState::differs ||
        !baseline.report.failed_assertions.empty() || !candidate.report.failed_assertions.empty() ? 1 : 0;
}
}

// 接收明确的离线执行和比较命令，错误进入标准错误输出。
// 步骤：匹配采集或比较 → 转换明确的允许变化字段 → 返回证据状态。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc == 3 && std::wstring_view(argv[1]) == L"--capture") return capture(argv[2]);
        if (argc >= 4 && argc <= 20 && std::wstring_view(argv[1]) == L"--compare") {
            std::vector<std::string> names;
            for (int index = 4; index < argc; ++index) {
                const auto utf = std::filesystem::path(argv[index]).u8string();
                names.emplace_back(utf.begin(),utf.end());
            }
            const std::vector<std::string_view> declared(names.begin(),names.end());
            return compare(argv[2],argv[3],declared);
        }
        throw std::invalid_argument("usage: --capture MATERIAL or --compare BASELINE CANDIDATE [DECLARED_FIELDS...]");
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}
