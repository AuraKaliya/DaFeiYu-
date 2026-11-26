#include "../decision_input.hpp"
#include "management/resident.hpp"
#include <fstream>
#include <iostream>

// 在正式命名管道上提供固定响应，可用隔离目录中的门闩延迟返回。
// 步骤：固定同目录启动身份 → 构造真实后台与离线模型 → 处理普通、延迟和失败问答。
int wmain(int argc, wchar_t** argv) {
    if (argc!=4) return 2;
    try {
        const std::filesystem::path config = argv[1], packages = argv[2], data = argv[3];
        return dfy::management::serve_resident(data,dfy::management::startup_identity(config,packages),[&] {
            return std::make_unique<dfy::app::Application>(dfy::model::Config{},packages,data,dfy::model::Backend{
                [&](const auto&,const dfy::model::Request& input) {
                    const auto text = dfy::test::business_messages(input).back().content;
                    if (text=="early-reply") return dfy::model::Response{{dfy::model::Role::assistant,"",
                        {{"speak","main_reply",R"({"text":"early question"})"}}},dfy::model::FinishReason::tool_calls};
                    const bool finishing = dfy::test::business_messages(input).back().role==dfy::model::Role::tool;
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
                }});
        });
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
