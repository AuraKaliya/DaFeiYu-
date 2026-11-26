#pragma once
#include <dafeiyu/runtime_ids.hpp>
#include <dafeiyu/observation_inbox.hpp>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace dfy::app {
struct RoomBinding {
    runtime::DomainId instance;
    std::uint64_t generation;
    // 同时核对本进程实例与代次，名称不参与身份判断。
    bool operator==(const RoomBinding&) const = default;
};
enum class RoomPlace { desk, window, behind_screen };
enum class RoomObject { desk_lamp, screen };
enum class RoomActivity { resting, walking };
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

struct RoomPerceptPage {
    std::uint64_t cursor;
    bool history_gap;
    std::vector<std::shared_ptr<const agent::ObservationRecord>> records;
};

enum class MovePhase { walking, arrived, blocked };
enum class MoveAdmission { accepted, duplicate, conflict, stale, busy, invalid };
struct RoomMove {
    std::uint64_t command;
    runtime::DomainId action;
    RoomPlace target;
    MovePhase phase;
    RoomSelf actual;
    agent::ObservationTime started, updated;
};
struct MoveReply { MoveAdmission admission; std::optional<RoomMove> record; };

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
    // 由原生身体来源更新姿态，重复修订只能重报相同位置、活动和采样时刻。
    bool accept_pose(const RoomSelf& sample, agent::ObservationTime sampled) noexcept;
    // 在事件发生时签发角色实际可感知的快照，迟到投递仍携带原来筛出的字段。
    std::optional<agent::ObservationSnapshot> capture_switch(LampSwitch change, std::uint64_t revision, agent::ObservationTime sampled);
    // 由原生台灯监测设备向绑定角色签发具名报告，报告的数值不写回房间灯态。
    std::optional<agent::ObservationSnapshot> capture_lamp_report(RoomBinding room, runtime::DomainId body, bool reported_on,
        std::uint64_t revision, agent::ObservationTime sampled);
    // 将原生已筛选的感知送入本角色有界记录，重复、过期和迟到沿用观察收件裁定。
    agent::ObservationReceipt receive(const agent::ObservationSnapshot& sample, agent::ObservationTime now);
    // 按当前姿态完整读取周围，一次主动读取不会生成新的被动事件或唤醒。
    std::shared_ptr<const agent::ObservationRecord> observe_current(RoomBinding room, runtime::DomainId body, agent::ObservationTime now);
    // 按角色自己的被动事件游标读取，淘汰较老记录后显式报告历史缺口。
    std::optional<RoomPerceptPage> percepts_since(RoomBinding room, runtime::DomainId body, std::uint64_t cursor) const;
    // 在原生房间空闲时安装某条既有通道的通行状态。
    bool set_passage_open(RoomBinding room, RoomPlace from, RoomPlace to, bool open) noexcept;
    // 接受一条原生移动命令，按原命令号区分重投、冲突和已过保留期。
    MoveReply start_move(RoomBinding room, runtime::DomainId body, std::uint64_t command, RoomPlace target, agent::ObservationTime now);
    // 以原动作的起步时刻推进身体，重复调度不重复走格。
    void advance_move(agent::ObservationTime now) noexcept;
    // 查询身体当前是否有在途动作，空闲轮询无需读取或推进场景时钟。
    bool walking() const noexcept { return resident_.activity==RoomActivity::walking; }
    // 仅向同房间、同身体交付原动作的最新或已退出回执。
    std::optional<RoomMove> movement(RoomBinding room, runtime::DomainId body, runtime::DomainId action) const noexcept;
  private:
    // 将角色可知的字段与当前身体修订交给原来源，签发具有有限时效的只读观察。
    agent::ObservationSnapshot sense_sample(std::string_view kind, bool from_desk, std::optional<bool> on, agent::ObservationTime sampled);
    RoomSelf resident_;
    agent::ObservationSource source_;
    agent::ObservationInbox inbox_;
    agent::ObservationSource monitor_;
    agent::ObservationInbox monitor_inbox_;
    std::optional<agent::ObservationSnapshot> last_report_;
    agent::ObservationTime scene_time_ = agent::ObservationTime::min(), pose_time_ = agent::ObservationTime::min();
    agent::ObservationTime switch_time_ = agent::ObservationTime::min();
    std::uint64_t switch_revision_ = 0, sense_revision_ = 0, received_ = 0;
    std::optional<bool> lamp_on_;
    std::optional<agent::ObservationSnapshot> last_switch_;
    std::array<std::shared_ptr<const agent::ObservationRecord>,32> history_{};
    std::array<std::optional<RoomMove>,32> moves_{};
    std::uint64_t last_command_ = 0;
    std::size_t move_slot_ = 0, walked_ = 0;
    std::vector<RoomPoint> route_;
    std::array<bool,2> open_passages_{true,true};
};
}
