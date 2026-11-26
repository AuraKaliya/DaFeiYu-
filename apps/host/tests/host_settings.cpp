#include "host_settings.hpp"
#include <windows.h>
#include <fstream>
#include <iostream>

namespace {
using namespace dfy::app;
namespace fs = std::filesystem;

// 报告主体设置存储与预期契约的差异。
void check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }

// 检查已退休的原提交、重启恢复的有界回执和未绑定目录的独立应用。
// 步骤：连续保存到保留上限以外 → 在原应用复查 → 从同一独立文件重建并核对。
void retention(const fs::path& profile) {
    int startup=2400;
    HostSettings settings(profile,"qwen",startup);
    for (unsigned index=1; index<=settings_receipt_limit+2; ++index) {
        const auto result=settings.save("intent-"+std::to_string(index),index,static_cast<int>(3000+index));
        check(result.state=="saved" && result.saved_revision==index+1 && result.current.applied_timeout_ms==2400,
            "bounded storage changed runtime or skipped a revision");
    }
    const auto latest=settings.snapshot();
    check(settings.receipt("intent-2").state=="unknown" && settings.receipt("intent-3").saved_revision==4 &&
        settings.save("intent-2",2,3002).state=="conflict","evicted submission executed again or displaced a retained receipt");
    HostSettings restarted(profile,"qwen",startup);
    check(startup==latest.timeout_ms && restarted.snapshot().applied_revision==latest.revision &&
        restarted.receipt("intent-3").saved_timeout_ms==3003,"restart did not recover the bounded user history");
    int injected_timeout=9;
    HostSettings detached({},"offline",injected_timeout);
    check(!detached.snapshot().can_save && detached.save("outside",1,1).state=="unavailable" && injected_timeout==9,
        "an independent application acquired a user settings writer");
}

// 用户文件损坏或运行中被替换时保留现场，独立启动值仍可用于应用。
void preserve_bad_file(const fs::path& profile) {
    const auto path=profile/"host-settings.json";
    int fallback=1800;
    HostSettings current(profile,"qwen",fallback);
    check(current.save("first",1,3600).state=="saved","could not seed user file");
    const std::string malformed=R"({"version":1,"version":2,"timeout_ms":42,"revision":"2","writes":[]})";
    std::ofstream(path,std::ios::binary|std::ios::trunc) << malformed;
    const auto failure=current.save("second",2,77);
    check(failure.state=="failed" && failure.current.timeout_ms==3600 && failure.current.revision==2,
        "corrupt disk contents caused a published revision");
    HostSettings restarted(profile,"qwen",fallback);
    check(!restarted.snapshot().available && !restarted.snapshot().can_save && fallback==1800 &&
        restarted.save("blocked",1,1).state=="unavailable","unreadable user file replaced the explicit startup value");
    std::ifstream input(path,std::ios::binary);
    check(std::string(std::istreambuf_iterator<char>(input),{})==malformed,"corrupt original was overwritten");
}
}

// 使用独立临时目录核对有界持久回执及不可读配置的原值保护。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc!=2) return 2;
        const auto root=fs::path(argv[1])/std::to_string(GetCurrentProcessId())/std::to_string(GetTickCount64());
        fs::create_directories(root/"history");
        fs::create_directories(root/"malformed");
        retention(root/"history");
        preserve_bad_file(root/"malformed");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
