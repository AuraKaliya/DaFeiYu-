#include "builtin_room.hpp"
#include <array>

namespace dfy::app {
namespace {
constexpr std::array places{
    KnownPlace{RoomPlace::desk,"书桌旁",4,0}, KnownPlace{RoomPlace::window,"窗边",0,0},
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
}

// 给新实例及其唯一角色分配身份，初始身体修订固定为一。
BuiltinRoom::BuiltinRoom()
    : resident_{{runtime::next_domain(),1},runtime::next_domain(),RoomPlace::desk,RoomActivity::resting,1} {}

// 返回当前实例绑定的第一版书房布局。
RoomLayout BuiltinRoom::layout() const noexcept { return {resident_.room,1,"书房",places,passages,objects}; }

// 检查调用者所见代次，成功时复制角色自身当前已知的位置与行为。
std::optional<RoomSelf> BuiltinRoom::self(RoomBinding expected) const noexcept {
    if (resident_.room != expected) return {};
    return resident_;
}
}
