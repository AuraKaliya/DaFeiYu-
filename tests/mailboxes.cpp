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
    rejects<std::invalid_argument>([&] { network.deliver(plugin,requests,1,request); });
    rejects<std::invalid_argument>([&] { network.read(app,requests); });
    rejects<std::invalid_argument>([&] { network.close(app,requests); });

    const auto question = network.deliver(app,requests,1,request).event;
    rejects<std::length_error>([&] { network.deliver(app,requests,2,request); });
    const auto reply = network.deliver(plugin,answers,1,answer).event;
    check(question == EventId{requests.value,1} && reply == EventId{answers.value,1},"mailboxes did not issue independent sequences");
    auto received = network.read(plugin,requests);
    check(received.event.get().id == question && received.event.get().payload->descriptor()->name() == "app.request",
        "request lost its class or identity");
    check(network.read(app,answers).event.get().id == reply,"reply was not addressed to the app");
    const auto status = message("app.status");
    check(network.deliver(app,requests,2,status).event.sequence == 2,"rejected delivery consumed a sequence");
    check(network.read(plugin,requests).event.get().payload->descriptor()->name() == "app.status",
        "mailbox lost a second message class");

    network.deliver(app,requests,3,request);
    network.close(plugin,requests);
    rejects<std::invalid_argument>([&] { network.deliver(app,requests,3,request); });
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
    network.deliver(app,original,1,payload);
    auto held = network.read(plugin,original);
    network.close(plugin,original);
    const auto replacement = network.open(plugin,app);
    rejects<std::length_error>([&] { network.deliver(app,replacement,1,payload); });
    held.event.reset();
    check(network.deliver(app,replacement,1,payload).event.sequence == 1,"released budget did not restore delivery");
}

// 验证丢失回执后的同号重投、过期回执与跳号均不会重复入队。
// 步骤：拒绝跳号 → 首次接纳并重投 → 读取首件并推进水位 → 检查过期重投。
void receipt_window() {
    Mailboxes network({1,1,2,0,1});
    const auto app = next_domain(), plugin = next_domain();
    const auto box = network.open(plugin,app);
    const auto request = message("app.request"), changed = message("app.changed");
    const auto ahead = network.deliver(app,box,2,request);
    check(ahead.state == DeliveryState::out_of_order && ahead.event == EventId{} &&
        network.read(plugin,box).state == PollState::empty,"out-of-order delivery entered the queue");

    const auto first = network.deliver(app,box,1,request);
    const auto repeat = network.deliver(app,box,1,changed);
    check(first.state == DeliveryState::accepted && repeat.state == DeliveryState::repeated && repeat.event == first.event,
        "lost ACK did not produce the original acceptance");
    check(network.read(plugin,box).event.get().payload == request && network.read(plugin,box).state == PollState::empty,
        "duplicate altered or repeated the original message");

    check(network.deliver(app,box,2,request).state == DeliveryState::accepted,"next sequence was not admitted");
    const auto expired = network.deliver(app,box,1,request);
    check(expired.state == DeliveryState::expired && expired.event == EventId{},"expired ACK appeared new");
    check(network.read(plugin,box).event.get().id.sequence == 2 && network.read(plugin,box).state == PollState::empty,
        "expired sequence was reaccepted");
}
}

// 运行定向信箱与生命周期检查。
int main() {
    try {
        directed_delivery();
        retained_budget();
        receipt_window();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
