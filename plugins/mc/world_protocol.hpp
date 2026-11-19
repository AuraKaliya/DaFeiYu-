#pragma once
#include <string>
#include <string_view>

namespace dfy::mc {
struct BridgeConfig { std::string channel, key; };
// 为配对的角色生成原世界的幂等接管请求。
std::string control_request(std::string_view id, std::string_view key, std::string_view arguments);
// 校验接管来源，尚未释放时返回空串供继续轮询。
std::string control_reply(std::string_view wire, std::string_view id, std::string_view arguments);
// 校验插件私有配置，配对键仅用于本机受信请求。
BridgeConfig configured_bridge(std::string_view config);
// 从独立插件配置中读取本机管道短名。
std::string configured_channel(std::string_view config);
// 创建带原请求身份的只读查询。
std::string world_request(std::string_view id);
// 核对版本、回显和状态形状，返回规范快照文本或表示撤销的 null。
std::string world_reply(std::string_view wire, std::string_view expected_id, bool with_survey = false);
// 为可信宿主生成包含探索依据的配对读请求。
std::string survey_request(std::string_view id, std::string_view key, std::string_view arguments);
// 用原游戏代次与游标生成带私有配对凭据的收件查询。
std::string inbox_request(std::string_view id, std::string_view key, std::string_view arguments);
// 只把与原查询相符且序号连续的玩家消息交给受信调用者。
std::string inbox_reply(std::string_view wire, std::string_view id, std::string_view arguments);
struct InspectReply { bool exited, arrived, moved; std::string payload; };
// 把已绑定的玩家上下文与模型提供的技能参数发送给配对的游戏角色。
std::string partner_request(std::string_view method, std::string_view id, std::string_view key,
    std::string_view skill, std::string_view arguments);
// 核对同一技能、世界及消息号，只在退出后交付包含实际效果的业务结果。
InspectReply partner_reply(std::string_view wire, std::string_view id, std::string_view skill, std::string_view arguments);
// 由宿主选择查看控制方法，模型参数只描述原消息与地标。
std::string inspect_request(std::string_view method, std::string_view id, std::string_view key, std::string_view arguments);
// 验证原世界及原委托的运动回执，成功到达必须有坐标和观察结果。
InspectReply inspect_reply(std::string_view wire, std::string_view id, std::string_view arguments);
// 生成对原观察的私有提问，问题号由宿主签发。
std::string question_request(std::string_view id, std::string_view key, std::string_view arguments);
// 校验原玩家、世界、查看委托与问题发送回执。
std::string question_reply(std::string_view wire, std::string_view id, std::string_view arguments);
// 验证完整委托并生成接单或只读回查帧。
std::string delivery_request(bool admit, std::string_view id, std::string_view key, std::string_view arguments);
// 核对原委托的接单回执，明确区分不可用、尚未接收和已接收。
std::string delivery_reply(bool admit, std::string_view wire, std::string_view id, std::string_view arguments);
struct MineReply { bool exited, finished, started; std::string payload; };
// 创建原采集委托内一个具体块编号的控制请求。
std::string mine_request(std::string_view method, std::string_view id, std::string_view key, std::string_view arguments);
// 核对采掘是否实际开始及客户端是否结束，持有证据另行判定。
MineReply mine_reply(std::string_view wire, std::string_view id, std::string_view arguments);
// 仅按已接单且数量明确的原委托提交或查询返程。
std::string return_request(std::string_view method, std::string_view id, std::string_view key, std::string_view arguments);
// 校验返程来源、位置、库存和可选的到达持有区间。
InspectReply return_reply(std::string_view wire, std::string_view id, std::string_view arguments);
}
