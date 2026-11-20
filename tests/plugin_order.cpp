#include <dafeiyu/plugin_order.hpp>
#include <iostream>
#include <stdexcept>

namespace {
using namespace dfy::plugins;
using namespace std::string_view_literals;

// 报告执行计划与声明不一致的具体位置。
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

// 验证共同与多重依赖，保持同一时刻可执行的注册顺序。
void stable() {
    const std::string_view needs_a[] = {"a"};
    const std::string_view needs_bc[] = {"b","c","b"};
    const PluginOrderNode registered[] = {{"d",needs_bc},{"c",needs_a},{"b",needs_a},{"a",{}},{"e",{}}};
    check(stable_plugin_order(registered) == std::vector<std::size_t>{3,1,2,0,4},
        "dependency or registration priority was lost");
    const PluginOrderNode independent[] = {{"z",{}},{"b",{}},{"a",{}}};
    check(stable_plugin_order(independent) == std::vector<std::size_t>{0,1,2},"independent nodes were reordered");
}

// 核对错误报告包含实际依赖环，并忽略只通向该环的外层节点。
void cycle() {
    const std::string_view a[] = {"a"}, b[] = {"b"}, c[] = {"c"};
    const PluginOrderNode registered[] = {{"free",{}},{"tail",a},{"a",b},{"b",c},{"c",a}};
    try { (void)stable_plugin_order(registered); }
    catch (const std::logic_error& error) {
        check(std::string_view(error.what()).ends_with("a -> b -> c -> a"sv),"cycle did not name the actual path");
        return;
    }
    throw std::runtime_error("cyclic registration was accepted");
}
}

// 运行稳定执行顺序和环路定位检查。
int main() {
    try {
        stable();
        cycle();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
