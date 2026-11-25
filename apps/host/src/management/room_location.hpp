#pragma once
#include "builtin_room.hpp"

namespace dfy::management {
// 原生房间和身体签发的位置摘要，名称来自不可变布局。
struct RoomLocation {
    app::RoomSelf self;
    std::uint64_t layout_version;
    std::string_view room_name, place_name;
};
}
