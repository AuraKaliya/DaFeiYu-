#pragma once
#include <dafeiyu/decision_status.hpp>
#include <dafeiyu/tool_schema.hpp>
#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <tuple>
#include <vector>

namespace dfy::agent::status_detail {
using Json = nlohmann::json;
struct Definition {
    Json declaration;
    ToolSchema schema;
    std::uint64_t version;
    std::size_t slot;
    bool explicit_attention;
};
using DefinitionRef = std::shared_ptr<const Definition>;
using Definitions = std::map<std::string,DefinitionRef>;
using OriginalId = std::tuple<std::string,std::string,std::uint64_t,std::uint64_t>;
using EventKey = std::pair<std::string,std::uint64_t>;
struct CountedWatch {
    std::string key, subject;
    std::uint64_t version, fact, count;
    // 同一来源的原事件号只能绑定到相同的原采样和展示次数。
    bool operator==(const CountedWatch&) const = default;
};
struct EventWatch {
    CountedWatch watch;
    std::uint64_t event, remaining;
    std::shared_ptr<std::uint64_t> retired;
    std::pair<std::uint64_t,std::uint64_t> last_decision{};
    bool published = false;
};
using EventRef = std::shared_ptr<EventWatch>;
using CountedWatches = std::map<EventKey,CountedWatch>;
// 由原状态存储在同一互斥区内使用，预览仅弱预留，最近发布的原事件可识别重传。
class EventAttention {
  public:
    // 验证同号原件并为本次冻结预留计次记录，淘汰窗口外的旧号不会重开关注。
    std::vector<EventRef> prepare(const CountedWatches& watches);
    // 请求已提交后用定长窗口保留原号，重复登记不推进窗口。
    void publish(const std::vector<EventRef>& events) noexcept;
  private:
    std::map<EventKey,std::weak_ptr<EventWatch>> index_;
    std::map<std::string,std::shared_ptr<std::uint64_t>> retired_;
    std::array<EventRef,128> recent_{};
    std::size_t next_ = 0;
};
struct Frame {
    Definitions definitions;
    std::map<std::pair<std::string,std::string>,Json> samples;
    std::map<std::pair<std::string,std::string>,std::set<std::string>> tasks;
    CountedWatches counted;
    std::map<OriginalId,std::string> originals;
    std::uint64_t cutoff = 0;
};
// 已提交的声明由绑定持有，尚未提交的同版候选共享弱预留，丢弃即可释放。
class Registry {
  public:
    // 按当前已发布注册和本次声明校验每个来源，保留各对象事实号最大的原采样。
    Frame collect(const std::vector<DecisionStatusSource>& sources);
    // 固定槽已在准备时选好，提交只转移不可变契约的共享所有权。
    void publish(const Definitions& definitions) noexcept;
  private:
    struct Slot { DefinitionRef committed; std::weak_ptr<const Definition> reserved; };
    std::array<Slot,64> slots_{};
    // 复用同版契约或预留新槽，拒绝命名空间冲突和不同的同版声明。
    DefinitionRef reserve(const std::string& owner, Json declaration);
};
}
