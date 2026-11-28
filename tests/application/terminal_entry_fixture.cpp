#include "../decision_input.hpp"
#include "management/resident.hpp"
#include <fstream>
#include <iostream>

// 在正式命名管道上提供固定响应，可用隔离目录中的门闩延迟返回。
// 步骤：固定同目录启动身份 → 构造真实后台与离线模型 → 按普通聊天或任务接续脚本应答。
int wmain(int argc, wchar_t** argv) {
    if (argc!=4 && argc!=5) return 2;
    try {
        const std::filesystem::path config = argv[1], packages = argv[2], data = argv[3];
        const std::filesystem::path trace = argc==5 ? std::filesystem::path(argv[4]) : std::filesystem::path{};
        return dfy::management::serve_resident(data,dfy::management::startup_identity(config,packages,trace),[&] {
            return std::make_unique<dfy::app::Application>(dfy::model::Config{},packages,data,dfy::model::Backend{
                [resume=!trace.empty(),&data](const auto&,const dfy::model::Request& input) {
                    const auto& message = dfy::test::business_messages(input).back();
                    const auto& text = message.content;
                    if (resume) {
                        if (message.role==dfy::model::Role::tool) return dfy::model::Response{{dfy::model::Role::assistant,"T03 checked in this run"}};
                        if (text!="Check the current T03 reading" || input.tools.size()!=1 || !input.tools.front().name.ends_with("_read"))
                            throw std::runtime_error("T03 did not offer its exact read scope");
                        return dfy::model::Response{{dfy::model::Role::assistant,"",{{"verify-t03",input.tools.front().name,"{}"}}},
                            dfy::model::FinishReason::tool_calls};
                    }
                    if (text=="early-reply") return dfy::model::Response{{dfy::model::Role::assistant,"",
                        {{"speak","main_reply",R"({"text":"early question"})"}}},dfy::model::FinishReason::tool_calls};
                    const bool finishing = message.role==dfy::model::Role::tool;
                    if (text=="delayed" || finishing) {
                        std::ofstream(data/(finishing ? "answer-entered" : "entered")) << "entered";
                        const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(10);
                        while (!std::filesystem::exists(data/(finishing ? "allow-final" : "release"))) {
                            if (std::chrono::steady_clock::now()>deadline) throw std::runtime_error("fixture gate expired");
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                        }
                    }
                    if (finishing) return dfy::model::Response{{dfy::model::Role::assistant,"early question"}};
                    if (text=="fail") throw std::runtime_error("fixed model failure");
                    return dfy::model::Response{{dfy::model::Role::assistant,"fixed:"+text}};
                }},dfy::app::ApplicationRunConfig{.trace_directory=trace});
        });
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
