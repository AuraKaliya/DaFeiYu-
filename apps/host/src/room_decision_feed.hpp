#pragma once
#include <dafeiyu/run_options.hpp>

namespace dfy::app {
class BuiltinRoom;
// 为这个房间的绑定角色与主会话建立独立的分派游标，房间寿命须覆盖该会话。
agent::DecisionObservationFeed room_decision_feed(const BuiltinRoom& room);
}
