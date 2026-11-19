#include "companion_messages.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace dfy::app;
using Side = CompanionSide;
using Kind = CompanionMessageKind;
// 对消息关联及窗口边界进行断言。
void check(bool ok) { if (!ok) throw std::runtime_error("companion message assertion"); }
// 确认非法消息不会推进游标或改变当前问题。
void rejects(const auto& action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid companion message accepted");
}
}
// 通过小窗口检验委托、回报、询问、答复及换世界后的旧消息。
// 步骤：建立关联事实 → 检查重发与方向 → 触发裁剪 → 更换绑定。
int main() {
    try {
        CompanionMessages messages(3);
        CompanionAddress address{1, 2, "world-one", 3};
        messages.bind(address);
        const auto task = messages.post(address, Side::primary, Kind::delegation, 1, 0, "去山脚看看");
        const auto report = messages.post(address, Side::game, Kind::report, 1, task.revision, "看见岩壁");
        check(messages.post(address, Side::game, Kind::report, 1, task.revision, "看见岩壁").revision == report.revision);
        rejects([&] { messages.post(address, Side::game, Kind::report, 1, task.revision, "新文本"); });
        rejects([&] { messages.post(address, Side::game, Kind::inquiry, 2, report.revision, "错方向"); });
        const auto question = messages.post(address, Side::primary, Kind::inquiry, 2, report.revision, "哪侧");
        rejects([&] { messages.post(address, Side::game, Kind::answer, 2, task.revision, "错问题"); });
        messages.post(address, Side::game, Kind::answer, 2, question.revision, "西侧");
        auto delta = messages.since(0);
        check(delta.gap && delta.oldest == 2 && delta.latest == 4 && delta.messages.size() == 3);
        check(!messages.since(1).gap && messages.since(4).messages.empty());
        rejects([&] { messages.post(address, Side::primary, Kind::delegation, 1, 0, "去山脚看看"); });
        messages.post(address, Side::game, Kind::report, 3, task.revision, "已更新");
        messages.bind(std::nullopt);
        rejects([&] { messages.post(address, Side::game, Kind::report, 4, task.revision, "迟到"); });
        rejects([&] { messages.bind(address); });
        const CompanionAddress next{1, 2, "world-two", 4};
        messages.bind(next);
        auto other = messages.post(next, Side::primary, Kind::delegation, 1, 0, "新世界");
        check(other.revision == 6 && other.task != task.task && messages.since(5).messages.size() == 1);
        rejects([&] { messages.since(7); });
        const auto entered = messages.post(next, Side::primary, Kind::event, 2, 0, "player.entered");
        const auto words = messages.post(next, Side::primary, Kind::utterance, 3, 0, "停，我来");
        check(!entered.task && !words.task && messages.task() == other.task);
        const auto spoken = messages.post(next, Side::game, Kind::report, 1, words.revision, "自选发言");
        check(!spoken.task && spoken.reply_to == words.revision);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
