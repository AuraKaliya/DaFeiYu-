#include "application.hpp"
#include <dafeiyu/companion_abi_v2.h>
#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>
#include <thread>

// 通过真实 DLL、命名管道与可控后端验证插件拥有一条完整的回复链路。
// 步骤：安装并启动原角色 → 等待在场和原话回合 → 核对真实技能回执 → 停止并卸载。
int wmain(int argc, wchar_t** argv) {
    using namespace dfy;
    using namespace model;
    try {
        if (argc != 3) throw std::runtime_error("package and test directory required");
        const std::filesystem::path root = argv[2];
        bool actual_reply = false;
        app::Application app({},root/"packages",root/"user",Backend{[&](const auto&,const Request& request) -> Response {
            if (request.tools.size() != 4) throw std::runtime_error("partner granted unintended tools");
            const auto& last = request.messages.back();
            if (last.role == Role::tool) {
                actual_reply = last.content.find("\"sent\":true") != std::string::npos;
                return {{Role::assistant,"internal settlement"}};
            }
            const auto input = nlohmann::json::parse(last.content);
            if (input.at("events").back().at("type") == "player.entered") return {{Role::assistant,"silent baseline"}};
            if (request.messages.size() != 4 || input.at("events").back().at("text") != "play with me")
                throw std::runtime_error("partner lost independent history or raw text");
            const auto reply = std::ranges::find_if(request.tools,[](const auto& tool) { return tool.name.ends_with("_reply"); });
            return {{Role::assistant,"",{{"answer",reply->name,R"({"text":"ready to play"})"}}},FinishReason::tool_calls};
        }});
        app.plugins().install(argv[1]);
        if (app.plugins().load("mc.companion")) throw std::runtime_error("could not load MC package");
        const auto instance = app.plugins().instance("mc.companion");
        {
            const auto api = instance->query<dfy_v2_companion_partner_api>(DFY_V2_COMPANION_PARTNER_INTERFACE,DFY_V2_COMPANION_PARTNER_VERSION);
            if (!api || instance->invoke(*api,[](const auto& fn, auto* native) { return fn.listen(native,nullptr); }))
                throw std::runtime_error("could not start plugin-owned partner");
            // 推进宿主直到原插件报告指定的状态边界。
            const auto wait = [&](bool stopped) {
                const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(8);
                while (std::chrono::steady_clock::now() < deadline) {
                    if (app.advance_plugins() == runtime::RunResult::failed) throw std::runtime_error("plugin loop failed");
                    dfy_v2_partner_status state{};
                    if (instance->invoke(*api,[&](const auto& fn, auto* native) { return fn.status(native,&state,nullptr); }))
                        throw std::runtime_error("partner state unavailable");
                    if (stopped ? state.phase == DFY_V2_PARTNER_CLOSED : state.ready && state.session && state.settled == 2) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                throw std::runtime_error("plugin did not reach expected boundary");
            };
            wait(false);
            if (!actual_reply || app.conversation().status()) throw std::runtime_error("reply missing or shared context");
            if (instance->invoke(*api,[](const auto& fn, auto* native) { return fn.stop(native,nullptr); }))
                throw std::runtime_error("could not stop plugin-owned partner");
            wait(true);
        }
        if (app.plugins().unload("mc.companion") != plugins::ShutdownResult::closed) throw std::runtime_error("MC unload failed");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
