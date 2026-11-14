#pragma once
#include "webview_support.hpp"
#include "frame_channel.hpp"
#include <nlohmann/json.hpp>
#include <functional>
#include <vector>

namespace dfy::ui {
// 仅由命令行验收模式启动的固定 CDP 检查，不向页面公开脚本执行接口或调试端口。
class InteractionProbe : public std::enable_shared_from_this<InteractionProbe> {
  public:
    // 固定浏览器及完成通知，在宿主 STA 内串行运行检查。
    InteractionProbe(ICoreWebView2* view, std::function<void(bool)> finished);
    // 准备固定测试序列并提交第一项；不操作系统鼠标、键盘或输入法设置。
    void start();
    // 验证多次真实 Frame 销毁、重建和旧视图拒绝，不操作桌面。
    void start_lifecycle(std::shared_ptr<const FrameChecks> checks);
    // 在原生计时器中继续等待释放条件，不阻塞 STA 消息循环。
    void advance() noexcept { if (!waiting_) next(); }
    // 使晚到完成回调失效并归还浏览器持有。
    void close() noexcept { closed_ = true; view_.Reset(); }
    // 返回已通过的协议检查数。
    std::size_t passed() const noexcept { return current_; }
  private:
    using Json = nlohmann::json;
    struct Step { std::string name; std::wstring method; Json parameters; bool frame; std::function<void(const Json&)> check; std::function<bool()> ready; };
    // 添加固定协议调用，不接收来自插件的脚本或方法名。
    void call(std::string name, std::wstring method, Json parameters, std::function<void(const Json&)> check = {});
    // 添加一个必须返回 true 的 DOM 断言，并选择主文档或已选 Frame。
    void evaluate(std::string name, std::string script, bool frame = false);
    // 有界等待只读页面条件，允许框架完成异步渲染，不重放操作。
    void expect(std::string name, std::string condition, bool frame = false);
    // 添加一对浏览器键盘事件，不向 Windows 注入键盘输入。
    void key(std::string name, std::string value, int code, int modifiers = 0);
    // 登记由宿主原生计数确认的等待条件，不接受页面自报释放。
    void wait_for(std::string name, std::function<bool()> ready);
    // 异步执行当前项，完成后验证并递交下一项，异常只使当前验收失败。
    void next() noexcept;
    ComPtr<ICoreWebView2> view_;
    std::function<void(bool)> finished_;
    std::vector<Step> steps_;
    std::size_t current_ = 0;
    std::string frame_id_;
    int context_ = 0;
    bool closed_ = false, waiting_ = false, completed_ = false;
};
}
