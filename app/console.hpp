#pragma once
#include <windows.h>
#include <cstdio>
#include <stdexcept>

namespace dfy::app {
class Utf8Console {
  public:
    // 临时启用控制台 UTF-8 输入输出，设置失败时恢复已改动的输入编码。
    // 步骤：保存原编码 → 设置 UTF-8 → 失败时回滚并报告错误。
    Utf8Console() : input_(GetConsoleCP()), output_(GetConsoleOutputCP()) {
        if (input_ && !SetConsoleCP(CP_UTF8)) {
            throw std::runtime_error("console: cannot set UTF-8 input");
        }
        if (output_ && !SetConsoleOutputCP(CP_UTF8)) {
            if (input_) {
                SetConsoleCP(input_);
            }
            throw std::runtime_error("console: cannot set UTF-8 output");
        }
    }

    // 退出作用域时刷新输出并尽力恢复原控制台编码，不改变重定向流的字节编码。
    // 步骤：写出仍以 UTF-8 编码的标准流缓冲 → 恢复原输入输出编码。
    ~Utf8Console() {
        // 标准流保持默认的 C/C++ 同步，刷新 C 缓冲也会写出 iostream 输出。
        std::fflush(stdout);
        std::fflush(stderr);

        if (input_) {
            SetConsoleCP(input_);
        }
        if (output_) {
            SetConsoleOutputCP(output_);
        }
    }

    // 禁止复制，避免重复恢复同一控制台设置。
    Utf8Console(const Utf8Console&) = delete;
    // 禁止赋值，保持编码恢复责任唯一。
    Utf8Console& operator=(const Utf8Console&) = delete;

  private:
    UINT input_, output_;
};
}
