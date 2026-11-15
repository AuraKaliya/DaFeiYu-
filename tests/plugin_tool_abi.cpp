#include <dafeiyu/plugin_instance.hpp>
#include <dafeiyu/tool_abi_v2.h>
#include <fstream>
#include <iostream>

namespace {
using namespace dfy::plugins;
namespace fs = std::filesystem;
// 报告实际 C DLL 的协议边界违约。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 创建独立包目录，测试不修改现有插件或用户设置。
// 步骤：取得新目录 → 复制 DLL 并写清单 → 读取正式包描述。
Manifest package(const fs::path& dll, const fs::path& root) {
    fs::create_directories(root); fs::path run;
    for (unsigned i = 1;; ++i) { run = root/std::to_string(i); if (fs::create_directory(run)) break; }
    fs::copy_file(dll,run/"entry.dll"); std::ofstream file(run/"plugin.json");
    file << R"({"manifest_version":1,"id":"test.tools","name":"tools","version":"1","abi_version":2,"entry":"entry.dll"})";
    file.close(); check(static_cast<bool>(file),"manifest write failed"); return load_manifest(run);
}
}
// 验证真实 C 工具的列举、接纳、延后退出、取消及票据释放。
// 步骤：创建活动实例 → 校验能力和描述 → 执行动作并延后释放 → 验证取消和旧票据 → 卸载。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"expected DLL and work directory"); const auto manifest = package(fs::canonical(argv[1]),argv[2]);
        dfy::platform::DynamicLibrary control(manifest.entry);
        const auto mode = reinterpret_cast<void (DFY_V2_CALL*)(uint32_t)>(control.symbol("dfy_tool_test_mode"));
        const auto count = reinterpret_cast<uint32_t (DFY_V2_CALL*)(uint32_t)>(control.symbol("dfy_tool_test_count"));
        auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,0),2,2);
        PluginInstance instance(manifest,manifest.directory/"settings",channel);
        check(!instance.create() && !instance.start(),"instance start failed");
        auto capability = instance.query<dfy_v2_tool_api>(DFY_V2_TOOL_INTERFACE,1);
        check(capability && !instance.query<dfy_v2_tool_api>(DFY_V2_TOOL_INTERFACE,2),"exact tool ABI unavailable");
        instance.invoke(*capability,
        // 在同一保护范围内复制借用结果并验证原生工作协议。
        // 步骤：读取描述 → 开始并推进实际动作 → 拒绝提前与失败释放 → 取消第二工作并拒绝旧票据。
        [&](const auto& api,auto* native) {
            uint32_t total = 0; check(!api.count(native,&total,nullptr) && total == 2,"tool count mismatch");
            dfy_v2_tool_description description{}; description.struct_size = sizeof(description);
            check(!api.describe(native,1,&description,nullptr) && description.flags == DFY_V2_TOOL_CHANGES_STATE &&
                std::string(description.id.data,static_cast<std::size_t>(description.id.length)) == "increment","tool description mismatch");
            dfy_v2_tool_call call{sizeof(call),1,1,1,1,1,1,{"call",4},{"{}",2}};
            dfy_v2_tool_ticket ticket{};
            check(!api.begin(native,{"increment",9},&call,&ticket,nullptr) && ticket.owner && ticket.sequence,"native admission failed");
            dfy_v2_tool_progress progress{}; progress.struct_size = sizeof(progress);
            check(!api.poll(native,ticket,&progress,nullptr) && progress.result_ready && !progress.exited && progress.effect == DFY_V2_TOOL_APPLIED &&
                std::string(progress.output.data,static_cast<std::size_t>(progress.output.length)) == "1","native effect or result missing");
            check(api.release(native,ticket,nullptr) == DFY_V2_BUSY,"native job released before exit");
            progress.struct_size = sizeof(progress); check(!api.poll(native,ticket,&progress,nullptr) && progress.exited,"native exit missing");
            mode(1); check(api.release(native,ticket,nullptr) == DFY_V2_FAILED && !api.release(native,ticket,nullptr),"failed release did not retain ticket");
            check(api.release(native,ticket,nullptr) == DFY_V2_INVALID_HANDLE,"released ticket remained valid");
            const auto old = ticket; call.invocation = 2;
            check(!api.begin(native,{"increment",9},&call,&ticket,nullptr) && ticket.sequence != old.sequence,"native slot reused ticket");
            progress.struct_size = sizeof(progress); check(api.poll(native,old,&progress,nullptr) == DFY_V2_INVALID_HANDLE && !progress.struct_size,"stale ticket reached new work");
            check(!api.request_cancel(native,ticket,DFY_V2_CANCEL_USER,nullptr),"cancel rejected");
            progress.struct_size = sizeof(progress);
            check(!api.poll(native,ticket,&progress,nullptr) && !progress.succeeded && progress.effect == DFY_V2_TOOL_NOT_APPLIED && progress.exited,
                "cancelled native work applied an effect");
            check(!api.release(native,ticket,nullptr) && count(0) == 2 && count(1) == 1 && count(2) == 2,"native work replayed or leaked");
        });
        check(instance.shutdown() == ShutdownResult::in_use,"capability did not keep code alive"); capability.reset();
        check(instance.shutdown() == ShutdownResult::closed,"native instance did not close");
        std::cout << "Native tool ABI checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
