#pragma once
#include <dafeiyu/runtime_ids.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

namespace dfy::app {
struct RoomBinding {
    runtime::DomainId instance;
    std::uint64_t generation;
    // 同时核对本进程实例与代次，名称不参与身份判断。
    bool operator==(const RoomBinding&) const = default;
};
enum class RoomPlace { desk, window, behind_screen };
enum class RoomObject { desk_lamp, screen };
enum class RoomActivity { resting };
struct RoomPoint { std::int16_t x, y; };
struct KnownPlace { RoomPlace id; std::string_view name; int x, y; };
struct RoomPassage { RoomPlace from, to; unsigned distance; };
struct KnownObject { RoomObject id; RoomPlace place; std::string_view name, use; };
// 首间书房的水平屏风占据闭区间，半径均使用布局的逻辑单位。
struct RoomSenses { unsigned sight_radius, hearing_radius; int screen_y, screen_left, screen_right; };

// 布局只包含角色熟悉的固定事实，视图指向本体内寿命覆盖整个进程的布局数据。
struct RoomLayout {
    RoomBinding room;
    std::uint64_t version;
    std::string_view name;
    std::span<const KnownPlace> places;
    std::span<const RoomPassage> passages;
    std::span<const KnownObject> objects;
    RoomSenses senses;
};
struct RoomSelf {
    RoomBinding room;
    runtime::DomainId body;
    RoomPlace place;
    RoomActivity activity;
    std::uint64_t revision;
    RoomPoint position;
};
struct LampSwitch { RoomBinding room; bool on; };
struct SeenLampChange { RoomPlace place; bool on; };
struct HeardSwitch { RoomPlace direction; };
// 声音只保留来向，只有亲见的变化能携带灯态。
using RoomPercept = std::variant<SeenLampChange,HeardSwitch>;

// 应用实例拥有的首间书房与主角色身体，所有读取在原业务线程完成。
class BuiltinRoom {
  public:
    // 为本次房间和绑定的身体分别签发唯一身份，角色从书桌旁开始。
    BuiltinRoom();
    // 实例身份与身体不能通过复制产生第二个所有者。
    BuiltinRoom(const BuiltinRoom&) = delete;
    // 已发布的实例身份保持固定。
    BuiltinRoom& operator=(const BuiltinRoom&) = delete;
    // 返回实例身份和共用的固定布局，读取不会推进环境。
    RoomLayout layout() const noexcept;
    // 仅为当前房间代次返回绑定身体的位置与行为。
    std::optional<RoomSelf> self(RoomBinding expected) const noexcept;
    // 以本体采样的身体位置筛选一次开关事件，返回该角色可知的内容。
    std::optional<RoomPercept> perceive_switch(const RoomSelf& sample, LampSwitch change) const noexcept;
  private:
    RoomSelf resident_;
};
}
