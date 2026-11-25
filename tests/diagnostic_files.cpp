#include "diagnostic_files.hpp"
#include "../modules/agent/tests/support/scenario_replay.hpp"
#include <fstream>
#include <iostream>

namespace {
using namespace dfy::agent;
using namespace dfy::tests;
// 报告本地诊断文件的保留、完整性或只读回顾错误。
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
// 验证已有文件、坏格式和超量输入明确拒绝。
void rejects(const auto& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("invalid diagnostic file operation accepted");
}
// 用单个固定响应准备无执行能力的诊断值。
DiagnosticSnapshot fixture() {
    ScriptStep step; step.last_text = "PRIVATE_FILE_GOAL"; step.response = dfy::model::Response{{dfy::model::Role::assistant,"PRIVATE_FILE_ANSWER"}};
    ReplayCase scenario; scenario.model = {1,"diagnostic_file",1,{},{step},1}; scenario.prompt = "PRIVATE_FILE_GOAL"; scenario.expected.history_messages = 2;
    return replay_scenario(scenario).diagnostic;
}
// 在明确测试目录下新建独立目录，已有条目不删除或覆盖。
std::filesystem::path directory(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    for (unsigned id = 1; id <= 1000; ++id) { const auto path = root/std::to_string(id); if (std::filesystem::create_directory(path)) return path; }
    throw std::runtime_error("test directory capacity exhausted");
}
}
// 实际读写中文路径，结束只移除本次明确创建的文件和空目录。
// 步骤：创建独立路径 → 完整发布并回顾 → 检查拒绝覆盖及错误输入 → 清理本次文件。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2,"expected test root"); const auto root = directory(argv[1]); const auto path = root/L"诊断 空格.json";
        const auto value = fixture(); dfy::app::write_diagnostic_file(path,value);
        const auto restored = dfy::app::read_diagnostic_file(path);
        check(encode_diagnostic(restored) == encode_diagnostic(value),"file round-trip changed facts");
        rejects([&] { dfy::app::write_diagnostic_file(path,value); });
        check(encode_diagnostic(dfy::app::read_diagnostic_file(path)) == encode_diagnostic(value),"existing diagnostic was overwritten");
        const auto bad = root/"bad.json", large = root/"large.json";
        { std::ofstream out(bad,std::ios::binary); out << "{}"; }
        rejects([&] { dfy::app::read_diagnostic_file(bad); });
        { std::ofstream out(large,std::ios::binary); out.seekp(8*1024*1024); out.put('x'); }
        rejects([&] { dfy::app::read_diagnostic_file(large); });
        rejects([&] { dfy::app::write_diagnostic_file(root/L"data.json:stream",value); });
        auto invalid = value; invalid.timeline.goal_revision = 0;
        rejects([&] { dfy::app::write_diagnostic_file(root/"invalid.json",invalid); });
        std::size_t count = 0; for (const auto& entry : std::filesystem::directory_iterator(root)) { ++count; check(entry.path().extension() == ".json","temporary file left behind"); }
        check(count == 3,"failed publication left unexpected files");
        std::filesystem::remove(path); std::filesystem::remove(bad); std::filesystem::remove(large); std::filesystem::remove(root);
        std::cout << "Diagnostic file checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
