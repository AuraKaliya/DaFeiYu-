#include <dafeiyu/budget.hpp>
#include <iostream>
#include <limits>
#include <vector>
namespace {
using namespace dfy::runtime;
// 在分层预算、回滚或归还违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 核对精确异常类型，保留无关失败供入口报告。
template<class Error, class Action>
void rejects(Action action) { try { action(); } catch (const Error&) { return; } throw std::runtime_error("budget accepted invalid charge"); }
// 验证多个接收方不能绕过所有者预算，失败不留下部分计费。
// 步骤：建立宿主与所有者层次 → 占满条目和字节 → 拒绝超额并核对原状态 → 移动归还并复用。
void hierarchy() {
    auto root = std::make_shared<Budget>(BudgetAmount{5,10});
    auto owner = std::make_shared<Budget>(BudgetAmount{3,8},root);
    Budget first({2,8},owner), second({2,8},owner), independent({4,10},root);
    auto a = first.acquire({2,3}); auto b = second.acquire({1,5});
    rejects<std::length_error>([&] { (void)second.acquire({1,0}); });
    rejects<std::length_error>([&] { (void)independent.acquire({1,3}); });
    check(root->used().items == 3 && root->used().bytes == 8 && owner->used().items == 3 && independent.used().items == 0,
        "rejected charge changed hierarchy");
    auto c = independent.acquire({2,2});
    rejects<std::length_error>([&] { (void)first.acquire({1,0}); });
    BudgetLease moved = std::move(a); check(!a && moved && root->used().items == 5,"move double charged budget");
    moved = std::move(b); check(!b && root->used().items == 3 && owner->used().bytes == 5,"move assignment failed to return previous charge");
    moved.reset(); moved.reset(); c.reset();
    check(!root->used().items && !root->used().bytes && !owner->used().items,"budget release leaked ancestor charge");
}
// 验证控制器退出后的凭证寿命、最大整数及固定层数。
// 步骤：保留子预算退出后的占用 → 检查无回绕边界 → 检查深度与非法参数。
void boundaries() {
    const auto maximum = std::numeric_limits<std::size_t>::max();
    auto root = std::make_shared<Budget>(BudgetAmount{maximum,maximum}); BudgetLease held;
    { Budget child({maximum,maximum},root); held = child.acquire({maximum,maximum}); }
    rejects<std::length_error>([&] { (void)root->acquire({1,0}); });
    check(root->used().items == maximum && root->used().bytes == maximum,"charge overflowed or owner destruction returned live lease");
    held.reset(); check(!root->used().items && !root->used().bytes,"orphan lease could not return root budget");
    std::vector<std::shared_ptr<Budget>> levels{root};
    for (int i = 1; i < 16; ++i) levels.push_back(std::make_shared<Budget>(BudgetAmount{1,0},levels.back()));
    rejects<std::invalid_argument>([&] { Budget too_deep({1,0},levels.back()); });
    rejects<std::invalid_argument>([] { Budget zero({0,0}); });
    rejects<std::invalid_argument>([&] { (void)root->acquire({0,1}); });
    auto leaf = levels.back()->acquire({1,0}); levels.clear(); root.reset(); leaf.reset();
}
}
// 验收分层原子计费、凭证移动、整数与寿命边界。
// 步骤：核对层次与回滚 → 核对极限及归还 → 报告结果。
int main() {
    try { hierarchy(); boundaries(); std::cout << "Budget checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
