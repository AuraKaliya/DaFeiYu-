#include "application.hpp"
#include "management/resident.hpp"
#include "fixtures/query_v2/mailbox_fixture.h"
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace dfy;
using Json = nlohmann::json;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

// 用真实测试插件发一封待收信件，记录固定模型的每次实际调用。
// 步骤：创建隔离应用 → 手动收件并加载插件 → 由原插件代次发送测试来信。
std::unique_ptr<app::Application> open_fixture(const fs::path& data) {
    auto app = std::make_unique<app::Application>(model::Config{},data/"plugins",data,model::Backend{
        [data](const auto&,const auto&) -> model::Response {
            std::ofstream(data/"fixed-model-calls",std::ios::app) << "called\n";
            return {{model::Role::assistant,"固定收件答复"}};
        }});
    app->wake_for_plugin_mail(false);
    if (app->plugins().load("test.mail")) throw std::runtime_error("fixture plugin could not load");
    const auto plugin = app->plugins().instance("test.mail");
    const auto entry = plugin->query<dfy_test_mailbox_api>(DFY_TEST_MAILBOX_INTERFACE,1);
    if (!entry || plugin->invoke(*entry,[](const auto& api, auto* instance) { return api.emit_notice(instance,1,0); })!=DFY_V2_OK)
        throw std::runtime_error("fixture plugin could not send its letter");
    return app;
}

// 测试控制端使用独立的终端身份申请收件或重载，两个桌面窗口只查询公开投影。
void control(const fs::path& data, const std::string& definition, bool reload) {
    const auto binding=management::connect_resident(data,definition);
    unsigned sequence=0;
    // 保持原命令号，在同一个终端请求上等待系统执行结果。
    const auto command = [&](const char* line, const char* expected) {
        const Json body{{"op","terminal"},{"client",binding.client},{"body",{{"id",++sequence},{"action","line"},{"text",line}}}};
        const auto end=std::chrono::steady_clock::now()+6s;
        while (std::chrono::steady_clock::now()<end) {
            const auto result=management::resident_request(data,body);
            if (!result.is_null()) {
                if (result.at("err")!="" || result.at("out").get_ref<const std::string&>().find(expected)==std::string::npos)
                    throw std::runtime_error(std::string("system mailbox command failed: ")+line+" "+result.dump());
                return;
            }
            std::this_thread::sleep_for(10ms);
        }
        throw std::runtime_error("system mailbox command timed out");
    };
    if (reload) {
        command("/plugins unload test.mail","Unloaded test.mail.");
        command("/plugins load test.mail","Loaded test.mail.");
    } else command("/mail review","Mailbox item reviewed.");
    management::resident_request(data,{{"op","disconnect"},{"client",binding.client}});
}
}

// 用正式后台入口运行固定响应应用，或从独立的控制进程变更原信件状态。
int wmain(int argc,wchar_t** argv) {
    try {
        if (argc!=4) return 2;
        const std::wstring_view mode=argv[1];
        const fs::path config=argv[2], data=argv[3];
        const auto definition=management::startup_identity(config,data/"plugins");
        if (mode==L"serve") return management::serve_resident(data,definition,[data] { return open_fixture(data); });
        if (mode!=L"review" && mode!=L"reload") return 2;
        control(data,definition,mode==L"reload");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
