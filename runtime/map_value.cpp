#include <dafeiyu/map_value.hpp>
#include <algorithm>

namespace dfy::runtime {
// 复用已实现的标量容器约束，不接受组合值占位。
MapDescriptor MapDescriptor::create(ValueKind kind, std::size_t count, std::size_t key_bytes, std::size_t value_bytes, bool nullable) {
    return MapDescriptor(ArrayDescriptor::create(kind, count, value_bytes, nullable), key_bytes);
}
// 构造有界映射，重复键或任何预算失败都不返回部分结果。
// 步骤：检查数量与节点 → 验证键和值并扣减正文 → 排序且拒绝重复 → 复制不可变条目。
MapValue MapValue::create(std::shared_ptr<const MapDescriptor> descriptor, std::span<const MapEntry> entries,
    std::size_t max_payload_bytes, std::size_t max_nodes) {
    if (!descriptor) throw std::invalid_argument("map: descriptor required");
    const auto nodes = std::min(max_nodes, Value::maximum_nodes);
    if (!nodes || entries.size() > descriptor->max_entries() || entries.size() > (nodes - 1) / 2) {
        throw std::length_error("map: entry or node limit exceeded");
    }

    auto remaining = max_payload_bytes;
    std::vector<const MapEntry*> ordered;
    ordered.reserve(entries.size());
    for (const auto& entry : entries) {
        if (entry.key.size() > descriptor->max_key_bytes() || entry.key.size() > remaining) throw std::length_error("map: key byte limit exceeded");
        (void)Value::from_string(entry.key, descriptor->max_key_bytes());
        remaining -= entry.key.size();
        if (entry.value.kind() == ValueKind::null_value) {
            if (!descriptor->nullable()) throw std::invalid_argument("map: null not allowed");
        } else if (entry.value.kind() != descriptor->value_kind()) throw std::invalid_argument("map: value kind mismatch");
        const auto bytes = entry.value.payload_bytes();
        if ((entry.value.kind() == ValueKind::string || entry.value.kind() == ValueKind::bytes) && bytes > descriptor->max_value_bytes()) {
            throw std::length_error("map: value byte limit exceeded");
        }
        if (bytes > remaining) throw std::length_error("map: payload limit exceeded");
        remaining -= bytes;
        ordered.push_back(&entry);
    }

    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) { return left->key < right->key; });
    for (std::size_t i = 1; i < ordered.size(); ++i) if (ordered[i - 1]->key == ordered[i]->key) throw std::invalid_argument("map: duplicate key");

    std::vector<MapEntry> owned;
    owned.reserve(ordered.size());
    for (const auto* entry : ordered) owned.push_back({entry->key, entry->value});
    return MapValue(std::move(descriptor), std::move(owned), max_payload_bytes - remaining);
}
// 在确定字节序中精确查找，不分配、转换或补默认值。
const Value* MapValue::find(std::string_view key) const noexcept {
    const auto found = std::lower_bound(entries_.begin(), entries_.end(), key,
        [](const auto& entry, std::string_view wanted) { return entry.key < wanted; });
    return found != entries_.end() && found->key == key ? &found->value : nullptr;
}
}
