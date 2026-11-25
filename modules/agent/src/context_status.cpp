#include <dafeiyu/context_status.hpp>
#include <dafeiyu/digest.hpp>
#include <dafeiyu/utf8.hpp>
#include <nlohmann/json.hpp>

namespace dfy::agent {
namespace {
using Json = nlohmann::json;
// 绑定和完整有序字段共同标识同一版数据，相同正文在不同绑定有独立身份。
std::string version(const ContextStatus& state) {
    return platform::sha256(Json::array({state.binding,state.fields}).dump());
}
}
// 投影宿主状态的首次基线、增减字段或同版引用，纯计算不会消耗变化。
// 步骤：检查绑定与字段上限 → 匹配原基线 → 记录变化和关闭项 → 编码来源与版本。
std::string project_context_status(const ContextStatus* previous, const ContextStatus& current) {
    if (current.binding.empty() || current.binding.size() > 256 || !runtime::valid_utf8(current.binding) || current.fields.size() > 32)
        throw std::invalid_argument("status: invalid binding or field count");
    std::size_t bytes = 0;
    for (const auto& [name,value] : current.fields) {
        if (name.empty() || name.size() > 80 || !runtime::valid_utf8(name) || !runtime::valid_utf8(value) ||
            value.size() > 4096 || (bytes += name.size()+value.size()) > 16*1024)
            throw std::invalid_argument("status: field outside limits");
    }
    Json out{{"source","host_status"},{"binding",current.binding},{"version",version(current)}};
    if (!previous || previous->binding != current.binding) {
        out["mode"] = "baseline";
        out["fields"] = current.fields;
    } else if (current.fields == previous->fields) {
        out["mode"] = "reference";
    } else {
        out["mode"] = "delta";
        out["from"] = version(*previous);
        auto changed = Json::object(), closed = Json::array();
        for (const auto& [key,value] : current.fields) {
            const auto old = previous->fields.find(key);
            if (old == previous->fields.end() || old->second != value) changed[key] = value;
        }
        for (const auto& [key,value] : previous->fields) if (!current.fields.contains(key)) closed.push_back(key);
        out["changed"] = std::move(changed);
        out["closed"] = std::move(closed);
    }
    return out.dump();
}
}
