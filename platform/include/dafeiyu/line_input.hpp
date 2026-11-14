#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace dfy::platform {
enum class InputState { empty, line, end, failed };
struct InputRead { InputState state; std::string line; std::uint32_t error = 0; };
// Windows 终端或重定向输入的有界后台读取，宿主始终非阻塞轮询。
class LineInput {
  public:
    // 借用标准输入，固定排队行数和单行 UTF-8 字节上限，保留系统终端编辑与回显。
    explicit LineInput(std::size_t capacity = 4, std::size_t max_line_bytes = 1024*1024);
    // 借用指定 Windows 输入句柄供管道及文件接入，调用方保持句柄直到 exited。
    LineInput(void* input, std::size_t capacity, std::size_t max_line_bytes);
    // 请求取消并等待专用读取线程退出，不关闭借用的输入句柄。
    ~LineInput();
    // 禁止复制读取线程及排队数据责任。
    LineInput(const LineInput&) = delete;
    // 禁止赋值或隐式移动正在使用的输入源。
    LineInput& operator=(const LineInput&) = delete;
    // 取得一行、暂空、结束或一次独立错误，EOF 前已排队数据仍按序交付。
    InputRead poll();
    // 请求取消读取及排队输入，不把取消调用返回当作线程已经退出。
    void close() noexcept;
    // 推进取消竞争及实际线程回收，不等待用户继续输入。
    void advance();
    // 判断专用读取线程已被宿主回收。
    bool exited() const noexcept;
  private:
    struct State;
    std::unique_ptr<State> state_;
};
}
