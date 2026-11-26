#include "../../../tests/decision_input.hpp"
#include "management/protocol.hpp"
#include "management/host.hpp"
#include "message_store.hpp"
#include <windows.h>
#include <iostream>
#include <semaphore>

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

// 等待宿主发布实际业务快照，每次检查均有独立期限。
template<class Ready> void wait_for(Ready ready) {
    const auto end = std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while (!ready()) {
        if (std::chrono::steady_clock::now()>=end) throw std::runtime_error("admission fixture timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// 从正式桌面分派核对持久接纳前的目标、字节容量以及超过旧显示字节上限的分页。
// 步骤：结束首轮并拒绝过期追加 → 在第二轮分别填满追加和队列预算 → 从固定水位翻回首条原文。
void admission(const std::filesystem::path& root) {
    auto db = std::make_shared<app::MessageStore>(root);
    std::binary_semaphore release(0);
    management::Host host([&] {
        return std::make_unique<app::Application>(model::Config{},root/"empty",root,model::Backend{[&](const auto&,const auto& request) {
            if (dfy::test::business_messages(request).back().content=="hold" && !release.try_acquire_for(std::chrono::seconds(60))) throw std::runtime_error("model gate expired");
            return model::Response{{model::Role::assistant,"fixed"}};
        }});
    },{},nullptr,db,"capacity");
    struct Finish {
        management::Host& host;
        std::binary_semaphore& gate;
        // 先停止后续接纳，再放行测试线程中的在途调用。
        ~Finish() { host.close(); gate.release(); }
    } finish{host,release};
    const auto window = host.open_client();
    const auto request = [&](Json body) {
        body.update(Json{{"version","1"},{"request","capacity"}});
        return management::dispatch(&host,body,window);
    };
    agent::RunId target{};
    const auto submit = [&](const char* type, unsigned id, const std::string& text) {
        Json body{{"type",type},{"command",std::to_string(id)},{"submission","case-"+std::to_string(id)},{"text",text}};
        if (body.at("type")!="dialogue.submit")
            body.update(Json{{"instance","capacity"},{"session",std::to_string(target.session)},{"turn",std::to_string(target.sequence)}});
        return request(std::move(body)).at("receipt").get<Json>();
    };
    wait_for([&] { return host.dialogue().accepting; });
    submit("dialogue.submit",1,"first");
    wait_for([&] { const auto view=host.dialogue(); return view.accepting && view.display->turns.size()==1; });
    target = host.dialogue().display->turns.back().run;
    if (submit("dialogue.append",2,"too late").at("state")!="stale" || db->find("desktop","case-2"))
        throw std::runtime_error("already closed turn was admitted to durable inbox");
    submit("dialogue.submit",3,"hold");
    wait_for([&] { return host.dialogue().display->turns.size()==2; });
    target = host.dialogue().display->turns.back().run;
    const std::string supplement(640*1024,'a'), queued(768*1024,'q');
    submit("dialogue.append",4,supplement);
    wait_for([&] { return host.dialogue().display->turns.back().inputs.size()==1; });
    if (submit("dialogue.append",5,supplement).at("state")!="full" || db->find("desktop","case-5"))
        throw std::runtime_error("over-budget supplement entered durable inbox");
    for (unsigned id : {6u,7u}) {
        submit("dialogue.enqueue",id,queued);
        wait_for([&] { return host.dialogue().display->queue.size()==id-5; });
    }
    if (submit("dialogue.enqueue",8,queued).at("state")!="full" || db->find("desktop","case-8"))
        throw std::runtime_error("over-budget next turn entered durable inbox");

    const auto subscription = request({{"type","messages.subscribe"}});
    Json page{{"type","messages.read"},{"conversation",subscription.at("conversation")},{"direction","history"},
        {"cursor","0"},{"through",subscription.at("watermark")},{"limit",64}};
    const auto recent = request(page);
    page["cursor"] = recent.at("next");
    const auto oldest = request(page);
    std::size_t bytes = 0;
    for (const auto* batch : {&recent,&oldest})
        for (const auto& record : batch->at("records")) bytes += record.at("text").get_ref<const std::string&>().size();
    if (recent.at("more")!=true || oldest.at("more")!=false || oldest.at("records").back().at("text")!="first" ||
        bytes!=supplement.size()+2*queued.size()+std::string("firstfixedhold").size() || bytes<=app::dialogue_display_byte_limit)
        throw std::runtime_error("persistent pagination lost input beyond display capacity");
}
}
// 使用当前构建目录与系统 SQLite 验收正式管理分派。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc!=2) return 2;
        const auto root = std::filesystem::path(argv[1])/std::to_string(GetCurrentProcessId())/std::to_string(GetTickCount64());
        verify(root/"windows");
        admission(root/"capacity");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
