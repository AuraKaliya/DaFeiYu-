#include <dafeiyu/run_loop.hpp>
#include <dafeiyu/session.hpp>
#include <algorithm>
#include <iostream>
#include <thread>

namespace {
using namespace dfy;
using namespace std::chrono_literals;

// 在第一个不符合正式请求或感知协议的断言处给出原因。
void check(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(reason);
}

// 在同一订阅游标上验证首次预算裁掉观察后可由后续正式分派接纳原文。
// 步骤：固定同一批次 → 用受限与正常窗口发起两轮 → 比较实际输入与消费水位。
void capacity() {
    const std::string text(10000,'a');
    const auto cursor=std::make_shared<std::uint64_t>(0);
    agent::RunOptions options;
    options.decision_observations=agent::DecisionObservationFeed{cursor,[&](auto after,auto,auto) -> std::optional<agent::DecisionObservationBatch> {
        if (after) return {};
        return agent::DecisionObservationBatch{1,text};
    }};
    const auto owner=runtime::next_domain();
    for (unsigned turn=1;turn<=2;++turn) {
        model::Config config;
        if (turn==1) { config.context_window_tokens=1024; config.reserved_output_tokens=128; }
        bool seen=false;
        agent::RunLoop loop({owner,turn},config,model::Backend{[&](const auto&,const model::Request& request) {
            seen=std::ranges::any_of(request.messages,[&](const auto& m) {
                return m.source==model::MessageSource::observation && m.content==text;
            });
            return model::Response{{model::Role::assistant,"完成"}};
        }},std::make_shared<agent::MessageHistory>(),"核对窗口",options);
        const auto deadline=std::chrono::steady_clock::now()+5s;
        while (loop.request()->exit()->state()==runtime::OperationState::pending && std::chrono::steady_clock::now()<deadline) {
            loop.advance(); std::this_thread::yield();
        }
        check(loop.request()->answer()=="完成" && seen==(turn==2) && *cursor==(turn==2 ? 1u : 0u),
            "capacity omitted the human request or consumed an unsent observation");
    }
}
}

// 用固定模型响应验收分派游标与请求预算。
int main() {
    try { capacity(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
