#include <dafeiyu/host_services.hpp>
#include <iostream>

namespace {
using namespace dfy::events;
using namespace dfy::plugins;
using namespace dfy::runtime;

// 指出发生偏差的信箱边界。
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

// 在当前绑定读取固定版本的 C 能力。
template<class Api>
const Api& service(std::string_view name) {
    const void* table = nullptr;
    check(HostServices::query({name.data(),name.size()},1,&table,nullptr) == DFY_V2_OK && table,"mailbox service unavailable");
    const auto& result = *static_cast<const Api*>(table);
    check(result.header.struct_size == sizeof(Api) && result.header.interface_version == 1,"incorrect capability layout");
    return result;
}

// 验证实例在 C API 中接入双向信箱，并在引用满时保留原首件。
// 步骤：在实例身份下接入并投递 → 按原能力读取与重试 → 关停并撤销地址。
void exchange() {
    auto network = std::make_shared<Mailboxes>(MailboxLimits{2,2,4,64});
    const auto app = next_domain(), plugin = next_domain();
    HostServices host(1);
    host.enable_mailboxes(network,app,plugin);
    auto binding = host.bind();
    const auto& api = service<dfy_v2_mailbox_api>(DFY_V2_MAILBOX_INTERFACE);
    const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
    dfy_v2_mailbox_pair addresses{}, same{};
    check(api.attach(&addresses,nullptr) == DFY_V2_OK && api.attach(&same,nullptr) == DFY_V2_OK &&
        addresses.inbound && same.inbound == addresses.inbound && same.outbound == addresses.outbound,"attachment not reused");

    dfy_v2_mailbox_receipt sent{};
    check(api.post(1,{"plugin.reply",12},2,{"hello",5},&sent,nullptr) == DFY_V2_OK &&
        sent.state == DFY_V2_MAILBOX_ACCEPTED && sent.mailbox == addresses.outbound,"plugin post failed");
    auto reply = network->read(app,{addresses.outbound});
    auto payload = reply.event.get().payload;
    check(payload->descriptor()->name() == "plugin.reply" && payload->descriptor()->version() == 2 &&
        payload->find("text")->as_string() == "hello","post lost its category or text");
    network->deliver(app,{addresses.inbound},1,payload);

    const auto occupied = host.expose(std::make_shared<const Value>(Value::null()));
    dfy_v2_mailbox_message read{};
    check(api.poll(&read,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !read.sequence,"failed read left a result");
    check(refs.release(occupied,nullptr) == DFY_V2_OK && api.poll(&read,nullptr) == DFY_V2_OK &&
        read.sequence == 1 && read.mailbox == addresses.inbound && read.payload.id,"failed read lost queued data");

    check(host.begin_shutdown() && host.mailbox_addresses().inbound == 0,"instance retained mailbox after stop");
    dfy_v2_mailbox_message rejected{};
    check(api.poll(&rejected,nullptr) == DFY_V2_UNSUPPORTED && refs.release(read.payload,nullptr) == DFY_V2_OK &&
        host.finish_shutdown(),"stop lost either revocation or already issued reference");
    check(network->open(app,plugin).value != addresses.outbound,"reopen reused the old plugin address");
}

// 确认第二条路线容量不足时归还第一条路线和输出身份。
void atomic_attach() {
    auto network = std::make_shared<Mailboxes>(MailboxLimits{1,1,1,64});
    const auto app = next_domain(), plugin = next_domain();
    HostServices host(1);
    host.enable_mailboxes(network,app,plugin);
    auto binding = host.bind();
    const auto& api = service<dfy_v2_mailbox_api>(DFY_V2_MAILBOX_INTERFACE);
    dfy_v2_mailbox_pair output{1,2};
    check(api.attach(&output,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !output.inbound && !output.outbound &&
        network->open(app,plugin).value,"attachment failure leaked an address");
}
}

// 执行绑定信箱及读取失败恢复测试。
int main() {
    try {
        exchange();
        atomic_attach();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
