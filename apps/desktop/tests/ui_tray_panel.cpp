#include "../src/interaction_probe.hpp"
#include <fstream>

namespace dfy::ui {
// 在被遮蔽的真实管理窗等待托盘投递确认，固定协议只负责观察或点击页面。
// 步骤：标记原生面板就绪 → 核对取消并继续观察 → 在第二次确认框内明确退出。
void InteractionProbe::start_tray_panel(const std::filesystem::path& profile) {
    const auto marker = profile / ("tray-check-" + std::to_string(GetCurrentProcessId()));
    expect("tray opened the connected panel", "document.querySelector('#dialogue-state')?.dataset.ready==='true'");
    wait_for("publish ready panel", [marker] { std::ofstream(marker.string()+".ready").put('1'); return true; });
    wait_for("first tray command", [marker] { return std::filesystem::exists(marker.string()+".first"); });
    expect("tray asks in the shared dialog", "document.querySelector('#host-stop-title').closest('dialog').open && document.activeElement.id==='host-stay'");
    evaluate("cancel tray exit", "document.querySelector('#host-stay').click(); !document.querySelector('#host-stop-title').closest('dialog').open");
    wait_for("publish cancellation", [marker] { std::ofstream(marker.string()+".cancelled").put('1'); return true; });
    wait_for("second tray command", [marker] { return std::filesystem::exists(marker.string()+".second"); });
    expect("second confirmation visible", "document.querySelector('#host-stop-title').closest('dialog').open");
    evaluate("confirm stop of the attached resident", "document.querySelector('#host-confirm-stop').click(); true");
    next();
}
}
