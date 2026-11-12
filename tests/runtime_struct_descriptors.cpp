#include <dafeiyu/struct_descriptor.hpp>
#include <dafeiyu/handle_registry.hpp>
#include <iostream>
#include <type_traits>

namespace {
using namespace dfy::runtime;
// 在描述快照违反契约时报告可定位的测试失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

// 验证拒绝使用预期错误类别，其他错误继续交由测试入口报告。
// 步骤：执行非法调用 → 捕获预期错误 → 对未拒绝的调用报告失败。
template<class Error, class Action>
void rejects(Action action) {
    try { action(); } catch (const Error&) { return; }
    throw std::runtime_error("descriptor: invalid input accepted");
}

// 验证六种字段的顺序、精确版本、输入隔离和通过生产引用表延长快照寿命。
// 步骤：从可变输入创建快照 → 修改并销毁输入 → 保留独立引用 → 读取并最终回收。
void verify_snapshot() {
    std::shared_ptr<const StructDescriptor> snapshot;
    {
        std::string name = "example.record";
        std::vector<FieldDescriptor> fields{{"flag", ValueKind::boolean, 0}, {"count", ValueKind::int64, 0},
            {"index", ValueKind::uint64, 0}, {"ratio", ValueKind::float64, 0},
            {"label", ValueKind::string, 8}, {"payload", ValueKind::bytes, 3}};
        snapshot = std::make_shared<const StructDescriptor>(StructDescriptor::create(name, 1, fields, {6, 45}));
        name.assign("changed");
        fields[4].name.assign("changed");
        fields[4].kind = ValueKind::bytes;
        fields[4].max_bytes = 999;
    }
    check(snapshot->name() == "example.record" && snapshot->version() == 1, "type identity changed");
    const auto fields = snapshot->fields();
    const ValueKind kinds[]{ValueKind::boolean, ValueKind::int64, ValueKind::uint64,
        ValueKind::float64, ValueKind::string, ValueKind::bytes};
    check(fields.size() == 6 && fields[0].name == "flag" && fields[5].name == "payload", "field order changed");
    for (std::size_t i = 0; i < fields.size(); ++i) check(fields[i].kind == kinds[i], "field kind changed");
    check(fields[4].name == "label" && fields[4].max_bytes == 8 && fields[5].max_bytes == 3, "input aliased");

    HandleRegistry<const StructDescriptor> references(2);
    const std::weak_ptr<const StructDescriptor> lifetime = snapshot;
    const auto first = references.insert(snapshot);
    const auto retained = references.retain(first);
    snapshot.reset();
    references.release(first);
    check(references.get(retained).fields()[4].name == "label", "retained descriptor lost");
    references.release(retained);
    check(lifetime.expired(), "descriptor leaked");
}

// 检查身份、字段及预算边界，零容量不绕过名称和种类校验。
// 步骤：验证空结构和边界版本 → 拒绝非法身份与字段 → 检查重复和预算耗尽。
void verify_rejections() {
    const auto empty = StructDescriptor::create("a.b", std::numeric_limits<std::uint32_t>::max(), {}, {0, 3});
    check(empty.fields().empty() && empty.version() == std::numeric_limits<std::uint32_t>::max(), "empty or version lost");
    for (const auto name : {"", "single", "a..b", ".a", "a.", "a.1b", "a.B", "a-b.c", "a.汉"}) {
        rejects<std::invalid_argument>([&] { (void)StructDescriptor::create(name, 1, {}, {0, 64}); });
    }
    rejects<std::invalid_argument>([] { (void)StructDescriptor::create(std::string_view("a.b\0c", 5), 1, {}, {0, 5}); });
    rejects<std::invalid_argument>([] { (void)StructDescriptor::create("a.b", 0, {}, {0, 3}); });
    rejects<std::length_error>([] { (void)StructDescriptor::create("a.b", 1, {}, {0, 0}); });

    std::vector<FieldDescriptor> fields{{"x", ValueKind::string, 0}};
    check(StructDescriptor::create("a.b", 1, fields, {1, 4}).fields()[0].max_bytes == 0, "zero byte bound lost");
    for (const auto name : {"", "X", "x.y", "_x", "1x", "x-y", "汉"}) {
        fields[0].name = name;
        rejects<std::invalid_argument>([&] { (void)StructDescriptor::create("a.b", 1, fields, {1, 64}); });
    }
    fields[0].name = std::string("x\0y", 3);
    rejects<std::invalid_argument>([&] { (void)StructDescriptor::create("a.b", 1, fields, {1, 64}); });
    fields[0] = {"x", static_cast<ValueKind>(255), 0};
    rejects<std::invalid_argument>([&] { (void)StructDescriptor::create("a.b", 1, fields, {1, 4}); });
    for (const auto kind : {ValueKind::boolean, ValueKind::int64, ValueKind::uint64, ValueKind::float64}) {
        fields[0] = {"x", kind, 1};
        rejects<std::invalid_argument>([&] { (void)StructDescriptor::create("a.b", 1, fields, {1, 4}); });
    }
    fields[0] = {"x_0", ValueKind::bytes, std::numeric_limits<std::size_t>::max()};
    check(StructDescriptor::create("a_0.b1", 1, fields, {1, 10}).fields()[0].max_bytes == fields[0].max_bytes,
        "descriptor allocated payload or changed byte bound");
    rejects<std::length_error>([&] { (void)StructDescriptor::create("a.b", 1, fields, {0, 64}); });
    rejects<std::length_error>([&] { (void)StructDescriptor::create("a.b", 1, fields, {1, 5}); });
    fields.push_back(fields[0]);
    rejects<std::invalid_argument>([&] { (void)StructDescriptor::create("a.b", 1, fields, {2, 9}); });
}
}

// 验收生产描述快照及错误边界，不构造结构载荷或注册类型目录。
// 步骤：核对不可变接口 → 验证快照寿命 → 验证拒绝规则 → 汇报结果。
int main() {
    try {
        static_assert(!std::is_copy_assignable_v<StructDescriptor> && std::is_copy_constructible_v<StructDescriptor>);
        static_assert(std::is_same_v<decltype(std::declval<StructDescriptor&>().fields()), std::span<const FieldDescriptor>>);
        verify_snapshot();
        verify_rejections();
        std::cout << "Runtime struct descriptor checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
