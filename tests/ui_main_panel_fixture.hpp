#pragma once
#include "application.hpp"
#include <atomic>
#include <semaphore>

namespace dfy::tests {
// 固定模型已进入原请求后，由原生验收序列决定迟到答复何时返回。
struct UiDialogueGate {
    std::atomic_uint entered = 0;
    std::atomic<runtime::DomainId> room = 0, body = 0;
    std::binary_semaphore release{0};
};

// 在独立空插件目录创建可控对话，用实际模型请求核对清空效果。
std::unique_ptr<app::Application> open_panel_fixture(const std::filesystem::path& work, std::shared_ptr<UiDialogueGate> gate);
}
