#include <dafeiyu/session.hpp>
#include <utility>

namespace dfy::agent {
// 保存模型配置，初始化空的内存会话。
Session::Session(model::Config config) : config_(std::move(config)) {}

// 在已有上下文上完成一次问答，失败不改变会话历史。
// 步骤：复制历史并追加提问 → 调用模型获取完整回答 → 成对提交问答并返回文本。
std::string Session::ask(const std::string& prompt) {
    auto pending = history_;
    pending.push_back({model::Role::user, prompt});

    auto answer = model::complete(config_, pending);

    // 先构造完整的新历史，再交换提交，避免异常留下半轮对话。
    pending.push_back({model::Role::assistant, answer});
    history_.swap(pending);
    return answer;
}

// 丢弃当前会话上下文，保留已选定的模型配置。
void Session::clear() { history_.clear(); }
}
