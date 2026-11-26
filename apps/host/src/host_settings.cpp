#include "host_settings.hpp"
#include "host_settings_file.hpp"
#include <algorithm>
#include <limits>

namespace dfy::app {
// 命令标识仅用于查回执，限制长度与字符后以原值保存。
bool valid_settings_submission(std::string_view text) noexcept {
    return !text.empty() && text.size()<=96 && std::ranges::all_of(text,[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_' || c=='.' || c=='/';
    });
}

// 配置文件不可读时保留显式启动值，停止提供可能覆盖原值的写入。
// 步骤：固定启动来源 → 加载用户覆盖与修订 → 将已确认超时交给本次应用构造。
HostSettings::HostSettings(const std::filesystem::path& profile, const std::string& provider, int& startup_timeout)
    : view_{provider,startup_timeout,startup_timeout} {
    if (profile.empty()) return;
    try {
        file_ = std::filesystem::absolute(profile).lexically_normal()/"host-settings.json";
        writes_ = read_host_settings_file(file_);
        if (!writes_.empty()) {
            const auto& saved = writes_.back();
            view_.user_timeout = true;
            view_.applied_revision = view_.revision = saved.revision;
            startup_timeout = view_.applied_timeout_ms = view_.timeout_ms = saved.timeout_ms;
        }
        view_.can_save = true;
    } catch (const std::exception&) { view_.available=false; }
}

// 在保存互斥范围内复制字段、修订和实际采用值。
HostSettingsView HostSettings::snapshot() const {
    std::lock_guard lock(mutex_);
    return view_;
}

// 先查原提交再校验基准，文件替换完成前不发布新修订。
// 步骤：防重并检查范围 → 比对内存和原用户文件 → 构造有界新回执 → 原子写入并公布。
HostSaveResult HostSettings::save(const std::string& submission, std::uint64_t base, int timeout_ms) {
    std::lock_guard lock(mutex_);
    if (!view_.can_save || view_.provider.empty()) return {"unavailable",0,view_};
    if (!valid_settings_submission(submission) || timeout_ms<host_timeout_min || timeout_ms>host_timeout_max) return {"invalid",0,view_};
    const auto prior = std::ranges::find(writes_,submission,&HostSettingWrite::submission);
    if (prior!=writes_.end()) {
        if (base==prior->revision-1 && timeout_ms==prior->timeout_ms) return {"saved",prior->revision,view_,prior->timeout_ms};
        return {"submissionConflict",0,view_};
    }
    if (base!=view_.revision) return {"conflict",0,view_};
    if (base==std::numeric_limits<std::uint64_t>::max()) return {"failed",0,view_};
    try {
        if (read_host_settings_file(file_)!=writes_) return {"storageChanged",0,view_};
        auto next=writes_;
        if (next.size()==settings_receipt_limit) next.pop_front();
        next.push_back({submission,base+1,timeout_ms});
        write_host_settings_file(file_,next,!writes_.empty());
        writes_.swap(next);
    } catch (const std::exception&) { return {"failed",0,view_}; }
    view_.revision=base+1;
    view_.timeout_ms=timeout_ms;
    view_.user_timeout=true;
    return {"saved",view_.revision,view_,timeout_ms};
}

// 按有界原提交记录返回当时已写入的修订，当前值另按当前修订提供。
HostSaveResult HostSettings::receipt(const std::string& submission) const {
    std::lock_guard lock(mutex_);
    if (!view_.available) return {"unavailable",0,view_};
    const auto prior=std::ranges::find(writes_,submission,&HostSettingWrite::submission);
    return prior==writes_.end() ? HostSaveResult{"unknown",0,view_} : HostSaveResult{"saved",prior->revision,view_,prior->timeout_ms};
}
}
