#include <dafeiyu/mailboxes.hpp>
#include <iostream>

namespace {
using namespace dfy::events;
using namespace dfy::runtime;

// 在投递或读取偏离信箱契约时报告失败。
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

// 验证预期请求按指定类别拒绝。
template<class Error, class Action>
void rejects(Action action) {
    try { action(); } catch (const Error&) { return; }
    throw std::runtime_error("mailboxes: invalid request accepted");
}

// 创建指定分类的不可变测试载荷。
std::shared_ptr<const StructValue> message(std::string_view name) {
    auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create(name,1,{}, {0,32}));
    return std::make_shared<const StructValue>(StructValue::create(std::move(type),{},0));
}

// 验证两个绑定地址各自按接纳顺序交付，并拒绝越权或过期地址。
// 步骤：建立双向信箱 → 拒绝越权和超量 → 对照分类和顺序 → 关闭并验证旧地址。
void directed_delivery() {
    Mailboxes network({2,1,3,0});
    const auto app = next_domain(), plugin = next_domain();
    const auto requests = network.open(plugin,app);
    const auto answers = network.open(app,plugin);
    const auto request = message("app.request"), answer = message("plugin.answer");
    rejects<std::length_error>([&] { network.open(plugin,app); });
    rejects<std::invalid_argument>([&] { network.deliver(plugin,requests,request); });
    rejects<std::invalid_argument>([&] { network.read(app,requests); });
    rejects<std::invalid_argument>([&] { network.close(app,requests); });

    const auto question = network.deliver(app,requests,request);
    rejects<std::length_error>([&] { network.deliver(app,requests,request); });
    const auto reply = network.deliver(plugin,answers,answer);
    check(question == EventId{requests.value,1} && reply == EventId{answers.value,1},"mailboxes did not issue independent sequences");
    auto received = network.read(plugin,requests);
    check(received.event.get().id == question && received.event.get().payload->descriptor()->name() == "app.request",
        "request lost its class or identity");
    check(network.read(app,answers).event.get().id == reply,"reply was not addressed to the app");
    const auto status = message("app.status");
    check(network.deliver(app,requests,status).sequence == 2,"rejected delivery consumed a sequence");
    check(network.read(plugin,requests).event.get().payload->descriptor()->name() == "app.status",
        "mailbox lost a second message class");

    network.deliver(app,requests,request);
    network.close(plugin,requests);
    rejects<std::invalid_argument>([&] { network.deliver(app,requests,request); });
    const auto replacement = network.open(plugin,app);
    check(replacement != requests && network.read(plugin,replacement).state == PollState::empty,"old queued data reached replacement");
    check(received.event.get().id == question,"closing a mailbox invalidated already delivered data");
}

// 检查已读消息在关闭信箱后继续计入总预算，释放后恢复接纳。
// 步骤：接纳并读出 → 关闭原地址并重开 → 验证背压后归还持有 → 重新投递。
void retained_budget() {
    Mailboxes network({1,1,1,0});
    const auto app = next_domain(), plugin = next_domain();
    const auto payload = message("app.request");
    const auto original = network.open(plugin,app);
    network.deliver(app,original,payload);
    auto held = network.read(plugin,original);
    network.close(plugin,original);
    const auto replacement = network.open(plugin,app);
    rejects<std::length_error>([&] { network.deliver(app,replacement,payload); });
    held.event.reset();
    check(network.deliver(app,replacement,payload).sequence == 1,"released budget did not restore delivery");
}
}

// 运行定向信箱与生命周期检查。
int main() {
    try {
        directed_delivery();
        retained_budget();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
