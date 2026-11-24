#include "management/local_pipe.hpp"
#include <future>
#include <iostream>

namespace {
using namespace std::chrono_literals;
using dfy::management::LocalPipe;
using dfy::management::local_request;

// 用真实管道核对目录别名、独占实例和跨缓冲区往返。
// 步骤：取得目录所有权 → 拒绝第二个所有者 → 连续交付两次独立有界交换 → 归还后重开端点。
void exercise(const std::filesystem::path& root) {
    const auto profile = root/"profile";
    const auto alias = profile/".";
    {
        LocalPipe owner(profile);
        try { LocalPipe other(alias); throw std::runtime_error("second owner accepted"); }
        catch (const std::system_error& duplicate) {
            if (duplicate.code().value()!=5 && duplicate.code().value()!=231) throw;
        }
        const auto echo = [](const std::string& text) { return "reply:"+text; };
        if (owner.serve_one(15ms,echo)) throw std::runtime_error("idle server accepted a client");
        for (const auto& input : {std::string("原生位置"),std::string(170000,'z')}) {
            auto server = std::async(std::launch::async,[&] { return owner.serve_one(3s,echo); });
            if (local_request(alias,input) != "reply:"+input || !server.get())
                throw std::runtime_error("pipe frame round trip failed");
        }
    }
    LocalPipe reopened(profile);
    auto server = std::async(std::launch::async,[&] {
        return reopened.serve_one(3s,[](const std::string&) { return "new owner"; });
    });
    if (local_request(profile,"read") != "new owner" || !server.get()) throw std::runtime_error("reopen failed");
}
}

// 在测试专用的用户目录验证本地传输与端点释放。
int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        exercise(argv[1]);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
