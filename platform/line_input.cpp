#include <dafeiyu/line_input.hpp>
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace dfy::platform {
struct LineInput::State {
    // 固定输入及缓冲容量，线程在全部准备成功后另行启动。
    State(HANDLE source, std::size_t capacity, std::size_t limit) : input(source), lines(capacity), max_line(limit) {
        if (!source || source == INVALID_HANDLE_VALUE || !capacity || !limit || limit > 1024*1024)
            throw std::invalid_argument("input: valid handle and bounded positive capacities required");
    }
    // 向固定队列转交完整行，过载等待可被停止唤醒，不增长额外队列。
    // 步骤：等待空位或停止 → 按停止状态分流 → 转交完整行并更新队列。
    bool publish(std::string line) {
        std::unique_lock lock(mutex); space.wait(lock,[&] { return stopping.load() || size < lines.size(); });
        if (stopping.load()) return false;
        lines[tail] = std::move(line); tail = (tail+1)%lines.size(); ++size; return true;
    }
    // 有界组装 UTF-8 行，保留跨读取分块，去除 Windows 行尾但不修改正文。
    // 步骤：逐段检查行长 → 遇到换行提交完整行 → 保留末尾未完成内容。
    bool append(std::string_view bytes, std::string& pending) {
        for (const char value : bytes) {
            if (value == '\n') {
                if (!pending.empty() && pending.back() == '\r') pending.pop_back();
                if (!publish(std::move(pending))) return false;
                pending.clear();
            } else {
                if (pending.size() >= max_line && !(pending.size() == max_line && value == '\r')) { error = ERROR_BUFFER_OVERFLOW; return false; }
                pending.push_back(value);
            }
        }
        return true;
    }
    // 将完整 UTF-16 片段严格转为 UTF-8，输入不含跨块尚未配对的高代理项。
    // 步骤：查询输出长度 → 准备完整结果 → 严格转换并核对长度。
    std::string utf8(const wchar_t* data, int count) {
        if (!count) return {};
        const int bytes = WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,data,count,nullptr,0,nullptr,nullptr);
        if (!bytes) throw std::runtime_error("input: invalid console text");
        std::string result(bytes,'\0');
        if (WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,data,count,result.data(),bytes,nullptr,nullptr) != bytes)
            throw std::runtime_error("input: console conversion failed");
        return result;
    }
    // 专用线程只读取和组装输入，不触碰会话、插件或宿主操作对象。
    // 步骤：选择终端或字节输入 → 按停止状态读取有界块 → 组装行或记录 EOF／错误 → 标记可回收。
    void run() noexcept {
        try {
            DWORD mode = 0; const bool console = GetConsoleMode(input,&mode) != 0; std::string pending; wchar_t high = 0;
            while (!stopping.load()) {
                DWORD count = 0; bool success, end; std::string bytes;
                if (console) {
                    wchar_t buffer[1024]; const DWORD prefix = high ? 1 : 0; if (high) buffer[0] = high;
                    success = ReadConsoleW(input,buffer+prefix,1024-prefix,&count,nullptr) != 0;
                    end = success && !count;
                    if (success && count) {
                        count += prefix; high = 0;
                        if (buffer[count-1] >= 0xd800 && buffer[count-1] <= 0xdbff) high = buffer[--count];
                        bytes = utf8(buffer,static_cast<int>(count));
                    }
                } else {
                    char buffer[4096]; success = ReadFile(input,buffer,sizeof(buffer),&count,nullptr) != 0;
                    end = success && !count;
                    if (success && count) bytes.assign(buffer,count);
                }
                if (stopping.load()) break;
                if (!success || end) {
                    const auto failure = success ? ERROR_HANDLE_EOF : GetLastError();
                    if (failure != ERROR_BROKEN_PIPE && failure != ERROR_HANDLE_EOF) error = failure;
                    else if (high) error = ERROR_NO_UNICODE_TRANSLATION;
                    else if (!pending.empty()) {
                        if (pending.back() == '\r') pending.pop_back();
                        (void)publish(std::move(pending));
                    }
                    break;
                }
                if (!append(bytes,pending)) break;
            }
        } catch (const std::bad_alloc&) { error = ERROR_NOT_ENOUGH_MEMORY; }
        catch (...) { error = ERROR_INVALID_DATA; }
        done.store(true,std::memory_order_release);
    }
    HANDLE input;
    std::vector<std::string> lines;
    const std::size_t max_line;
    std::size_t head = 0, tail = 0, size = 0;
    std::mutex mutex;
    std::condition_variable space;
    std::atomic<bool> stopping{false}, done{false};
    DWORD error = 0;
    bool reported = false, joined = false;
    std::thread thread;
};
// 读取系统标准输入，不替换或关闭其句柄。
LineInput::LineInput(std::size_t capacity, std::size_t max_line_bytes) : LineInput(GetStdHandle(STD_INPUT_HANDLE),capacity,max_line_bytes) {}
// 在完整状态准备后启动独立读取线程，失败不留下后台执行。
// 步骤：完整准备有界状态 → 启动并保存专用读取线程。
LineInput::LineInput(void* input, std::size_t capacity, std::size_t max_line_bytes)
    : state_(std::make_unique<State>(input,capacity,max_line_bytes)) { state_->thread = std::thread([state = state_.get()] { state->run(); }); }
// 反复推进取消以覆盖读调用之间的竞争，实际退出前不销毁读取缓冲。
// 步骤：请求停止 → 反复推进取消与退出 → 保持状态到线程已回收。
LineInput::~LineInput() { close(); while (!exited()) { advance(); if (!exited()) Sleep(1); } }
// 停止标志和满队列唤醒独立于 I/O 取消，排队输入在请求停止时丢弃。
// 步骤：固定停止标志 → 清空待交付输入 → 唤醒队列等待 → 请求取消同步读取。
void LineInput::close() noexcept {
    state_->stopping.store(true);
    { std::lock_guard lock(state_->mutex); for (auto& line : state_->lines) line.clear(); state_->size = 0; }
    state_->space.notify_all();
    if (state_->thread.joinable()) CancelSynchronousIo(state_->thread.native_handle());
}
// 取消请求可能发生在两次读取之间，持续推进再次取消；只有 join 后才确认资源退出。
// 步骤：检查已回收状态 → 按工作完成或停止请求分流 → 确认回收或重试取消。
void LineInput::advance() {
    if (state_->joined) return;
    if (state_->done.load(std::memory_order_acquire)) { state_->thread.join(); state_->joined = true; }
    else if (state_->stopping.load()) CancelSynchronousIo(state_->thread.native_handle());
}
// 非阻塞交付一行，先排空已接纳数据，再报告错误或最终结束。
// 步骤：推进实际退出 → 取出队首并唤醒生产者 → 返回暂空或一次错误及结束。
InputRead LineInput::poll() {
    advance(); std::lock_guard lock(state_->mutex);
    if (state_->size) {
        auto line = std::move(state_->lines[state_->head]); state_->head = (state_->head+1)%state_->lines.size(); --state_->size;
        state_->space.notify_one(); return {InputState::line,std::move(line),0};
    }
    if (!state_->joined) return {InputState::empty,{}};
    if (state_->error && !state_->reported && !state_->stopping.load()) { state_->reported = true; return {InputState::failed,{},state_->error}; }
    return {InputState::end,{}};
}
// 仅宿主线程读取回收确认，不把取消或工作线程临近退出当作已经 join。
bool LineInput::exited() const noexcept { return state_->joined; }
}
