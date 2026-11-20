#include <dafeiyu/plugin_manager.hpp>
#include "fixtures/query_v2/mailbox_fixture.h"
#include <fstream>
#include <iostream>

namespace {
using namespace dfy::events;
using namespace dfy::plugins;
using namespace dfy::runtime;
namespace fs = std::filesystem;

// 报告跨 DLL 读写和实例销毁的具体失败点。
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

// 构建本次独立测试包，保留其他运行的目录。
fs::path package(const fs::path& root, const fs::path& dll) {
    fs::create_directories(root);
    for (unsigned index = 1;; ++index) {
        const auto path = root/std::to_string(index)/"plugins"/"mail";
        if (!fs::create_directories(path)) continue;
        fs::copy_file(dll,path/"entry.dll");
        std::ofstream manifest(path/"plugin.json");
        manifest << R"({"manifest_version":1,"id":"test.mail","name":"mailbox example","version":"1","abi_version":2,"entry":"entry.dll"})";
        return path.parent_path();
    }
}
}

// 验证从正式插件管理器载入的 C DLL 可以收到本体请求并回信。
// 步骤：加载并定位信箱 → 投递与回信 → 检查实际分类 → 卸载并阻止旧地址复用。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"mailbox DLL and workspace required");
        const auto root = package(argv[2],argv[1]);
        PluginMailboxHost host{std::make_shared<Mailboxes>(MailboxLimits{2,2,4,64}),next_domain()};
        auto channel = std::make_shared<BroadcastChannel>(std::make_shared<EventStore>(8,64),2,2);
        PluginManager manager(root,root.parent_path()/"data",channel,1,host);
        check(manager.load("test.mail") == DFY_V2_OK,"mailbox plugin failed to attach at load");
        const auto instance = manager.instance("test.mail");
        const auto addresses = instance->mailbox_addresses();
        check(addresses.inbound && addresses.outbound,"manager lost registered mailbox addresses");

        auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.request",1,{}, {0,32}));
        auto body = std::make_shared<const StructValue>(StructValue::create(std::move(type),{},0));
        check(host.network->deliver(host.identity,{addresses.inbound},1,std::move(body)).state == DeliveryState::accepted,
            "host request was not admitted");
        {
            const auto reply = instance->query<dfy_test_mailbox_api>(DFY_TEST_MAILBOX_INTERFACE,1);
            check(reply && instance->invoke(*reply,[](const auto& api, auto* plugin) { return api.reply(plugin,nullptr); }) == DFY_V2_OK,
                "actual C plugin could not poll and post");
        }
        auto answer = host.network->read(host.identity,{addresses.outbound});
        check(answer.event.get().payload->descriptor()->name() == "test.reply" &&
            answer.event.get().payload->find("text")->as_string() == "received","host received invalid reply");
        check(manager.unload("test.mail") == ShutdownResult::closed && !instance->mailbox_addresses().inbound,"mailbox blocked clean unload");
        check(manager.load("test.mail") == DFY_V2_OK && manager.instance("test.mail")->mailbox_addresses().outbound != addresses.outbound,
            "reload reused old mailbox routing");
        check(manager.close(),"mailbox manager did not close");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
