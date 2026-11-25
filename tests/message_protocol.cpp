#include "management/protocol.hpp"
#include "management/host.hpp"
#include "message_store.hpp"
#include <windows.h>
#include <iostream>

namespace {
using namespace dfy;
using Json = nlohmann::json;
// 验证两个窗口在旧水位分页期间仍可补读新收件和原消息的回执修订。
// 步骤：登记首窗与固定快照 → 写入跨页变化 → 比对两个窗口的原记录 → 独立补读。
void verify(const std::filesystem::path& root) {
    auto db = std::make_shared<app::MessageStore>(root);
    management::Host host([&] { return std::make_unique<app::Application>(model::Config{},root/"plugins",root,
        model::Backend{[](const auto&,const auto&) { return model::Response{{model::Role::assistant,"fixed"}}; }}); },{},nullptr,db,"original");
    const auto first = host.open_client(), second = host.open_client();
    const auto call = [&](const auto& client, Json body) {
        body.update(Json{{"version","1"},{"request","fixture"}});
        return management::dispatch(&host,body,client);
    };
    for (int i=0; i<19; ++i)
        (void)db->put("desktop",std::to_string(i),{{"text","same text"}},{{"state","pending"}});
    const auto original = call(first,{{"type","messages.subscribe"}});
    Json page{{"type","messages.read"},{"conversation",original.at("conversation")},{"direction","history"},
        {"cursor","0"},{"through",original.at("watermark")},{"limit",8}};
    const auto recent = call(first,page);
    if (recent.at("records").size()!=8 || recent.at("more")!=true || recent.at("records").front().at("message")!="19")
        throw std::runtime_error("wrong history page");
    db->update(1,{{"state","delivered"}});
    (void)db->put("main","reply",{{"text","new"}},{{"state","delivered"}});
    if (call(second,{{"type","messages.subscribe"}}).at("watermark")!="21") throw std::runtime_error("second subscription missed commit");
    for (const auto& window : {first,second}) {
        page["cursor"] = recent.at("next");
        const auto middle = call(window,page);
        page["cursor"] = middle.at("next");
        const auto oldest = call(window,page);
        if (oldest.at("records").size()!=3 || oldest.at("records").back().at("receipt").at("state")!="pending" || oldest.at("more")!=false)
            throw std::runtime_error("old snapshot changed while paging");
        Json delta = page;
        delta.update(Json{{"cursor","19"},{"through","21"},{"direction","changes"}});
        const auto updates = call(window,delta);
        if (updates.at("records").size()!=2 || updates.at("records")[0].at("message")!="1" ||
            updates.at("records")[0].at("receipt").at("state")!="delivered" || updates.at("next")!="21")
            throw std::runtime_error("independent catch-up missed or consumed a receipt");
        if (call(window,{{"type","dialogue.read"},{"command",""},{"revision","unchanged"}}).at("messageWatermark")!="21")
            throw std::runtime_error("poll missed message watermark");
    }
}
}
// 使用当前构建目录与系统 SQLite 验收正式管理分派。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc!=2) return 2;
        verify(std::filesystem::path(argv[1])/std::to_string(GetCurrentProcessId())/std::to_string(GetTickCount64()));
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
