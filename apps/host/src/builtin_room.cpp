#include "builtin_room.hpp"
#include <array>

namespace dfy::app {
namespace {
constexpr RoomPoint desk_position{4,0};
constexpr KnownPlace desk{RoomPlace::desk,"书桌旁",desk_position.x,desk_position.y};
constexpr std::array places{
    desk, KnownPlace{RoomPlace::window,"窗边",0,0},
    KnownPlace{RoomPlace::behind_screen,"屏风后",4,3}
};
// 首版的连接均可双向步行，距离使用房间逻辑单位。
constexpr std::array passages{
    RoomPassage{RoomPlace::desk,RoomPlace::window,4}, RoomPassage{RoomPlace::desk,RoomPlace::behind_screen,3}
};
constexpr std::array objects{
    KnownObject{RoomObject::desk_lamp,RoomPlace::desk,"台灯","照亮书桌"},
    KnownObject{RoomObject::screen,RoomPlace::behind_screen,"屏风","遮挡视线"}
};
constexpr RoomSenses senses{3,4,2,3,5};

// 判断从台灯到采样位置的线段是否经过水平屏风，碰到边缘同样遮挡。
bool screen_blocks_lamp(RoomPoint observer) noexcept {
    if (observer.y < senses.screen_y) return false;
    const int rise = observer.y - desk.y;
    // 同乘正数 rise，以整数比较交点，保证屏风边缘的判定一致。
    const auto crossing = desk.x * rise + (observer.x - desk.x) * (senses.screen_y - desk.y);
    return senses.screen_left * rise <= crossing && crossing <= senses.screen_right * rise;
}
}

// 给新实例及其唯一角色分配身份，初始身体修订固定为一。
BuiltinRoom::BuiltinRoom()
    : resident_{{runtime::next_domain(),1},runtime::next_domain(),RoomPlace::desk,RoomActivity::resting,1,desk_position},
      source_(resident_.room.instance,agent::ObservationTrust::host_verified),inbox_({source_.id()},1),
      monitor_(resident_.room.instance,agent::ObservationTrust::host_verified),monitor_inbox_({monitor_.id()},1) {}

// 返回当前实例绑定的第一版书房布局。
RoomLayout BuiltinRoom::layout() const noexcept { return {resident_.room,1,"书房",places,passages,objects,senses}; }

// 检查调用者所见代次，成功时复制角色自身当前已知的位置与行为。
std::optional<RoomSelf> BuiltinRoom::self(RoomBinding expected) const noexcept {
    if (resident_.room != expected) return {};
    return resident_;
}

// 计算该身体对一次台灯变化可见或可听的内容。
// 步骤：核对事件与身体身份 → 比较逻辑距离及屏风遮挡 → 只返回可辨认的事实。
std::optional<RoomPercept> BuiltinRoom::perceive_switch(const RoomSelf& sample, LampSwitch change) const noexcept {
    if (change.room != resident_.room || sample.room != resident_.room || sample.body != resident_.body) return {};

    const std::int64_t dx = sample.position.x - desk.x;
    const std::int64_t dy = sample.position.y - desk.y;
    const auto squared_distance = dx * dx + dy * dy;
    if (squared_distance <= senses.sight_radius * senses.sight_radius && !screen_blocks_lamp(sample.position))
        return SeenLampChange{desk.id,change.on};
    if (squared_distance <= senses.hearing_radius * senses.hearing_radius) return HeardSwitch{desk.id};
    return {};
}
}
