#include "control.hpp"
#include <chrono>
#include <fstream>
#include <random>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace dfy::host {
namespace {
// 生成控制端点代次和请求文件名使用的随机令牌。
std::string nonce() {
    std::random_device source;
    std::string result;
    constexpr char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i)
        result += digits[source() & 15];
    return result;
}

// 读取有大小限制的控制消息，并兼容 Windows 原子发布后的文件共享。
// 步骤：按平台打开文件 → 校验消息大小 → 完整读取字节 → 解析 JSON 请求或响应。
Json read(const Path& file) {
#ifdef _WIN32
    // 重命名发布后原句柄仍可能持有删除访问权，读取时需允许共享删除。
    struct FileHandle {
        HANDLE value;

        // 关闭有效的 Windows 控制消息文件句柄。
        ~FileHandle() {
            if (value != INVALID_HANDLE_VALUE)
                CloseHandle(value);
        }
    } handle{CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};

    if (handle.value == INVALID_HANDLE_VALUE)
        throw std::runtime_error("CONTROL_READ: " + std::to_string(GetLastError()));

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.value, &size) || size.QuadPart < 0 || size.QuadPart > 1024 * 1024)
        throw std::runtime_error("CONTROL_MESSAGE_SIZE");
    std::string body(static_cast<size_t>(size.QuadPart), '\0');

    size_t offset = 0;
    while (offset < body.size()) {
        DWORD received{};
        if (!ReadFile(
                handle.value, body.data() + offset, static_cast<DWORD>(body.size() - offset), &received, nullptr) ||
            received == 0)
            throw std::runtime_error("CONTROL_READ: " + std::to_string(GetLastError()));
        offset += received;
    }

    return Json::parse(body);
#else
    if (std::filesystem::file_size(file) > 1024 * 1024)
        throw std::runtime_error("CONTROL_MESSAGE_TOO_LARGE");
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
        throw std::runtime_error("CONTROL_READ");
    return Json::parse(stream);
#endif
}

// 将控制消息写入临时文件，再通过重命名发布完整内容。
void write(const Path& target, const Json& value) {
    auto temporary = target;
    temporary += ".tmp-" + nonce();

    {
        std::ofstream stream(temporary, std::ios::binary);
        stream << value.dump();
        stream.flush();
        if (!stream)
            throw std::runtime_error("CONTROL_WRITE");
    }

    std::filesystem::rename(temporary, target);
}
}

// 运行本地文件控制通道，将客户端请求转交常驻实例。
// 步骤：发布当前端点代次 → 轮询并校验请求 → 执行命令及发布响应 → 收到停止信号后撤销端点并关闭实例。
void serve(InstanceRuntime& runtime, const Path& directory, const std::atomic<bool>& interrupted) {
    const auto control = directory / "control";
    std::filesystem::create_directories(control);

    const auto epoch = nonce();
    const auto endpoint = control / "endpoint.json";
    if (std::filesystem::exists(endpoint))
        std::filesystem::remove(endpoint);
    write(endpoint, {{"epoch", epoch}, {"protocol", 1}});

    bool stopping = false;
    while (!interrupted.load() && !stopping) {
        for (const auto& entry : std::filesystem::directory_iterator(control)) {
            const auto name = sdk::path_text(entry.path().filename());
            if (!entry.is_regular_file() || !name.ends_with(".request.json"))
                continue;
            Json response;

            try {
                const auto request = read(entry.path());
                if (request.at("epoch") != epoch)
                    throw std::runtime_error("CONTROL_EPOCH_EXPIRED");

                auto args = request.at("arguments");
                args["_command_id"] = request.at("command_id");
                const auto command = request.at("command").get<std::string>();
                response = {{"ok", true}, {"result", runtime.request(command, args)}};
                if (command == "host.shutdown")
                    stopping = true;
            } catch (const std::exception& error) {
                response = {{"ok", false}, {"error", error.what()}};
            }

            const auto response_path =
                control / sdk::path_from_utf8(name.substr(0, name.size() - 13) + ".response.json");
            if (!std::filesystem::exists(response_path))
                write(response_path, response);
            std::filesystem::remove(entry.path());
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::filesystem::remove(endpoint);
    runtime.shutdown();
}

// 向当前实例端点提交命令，并等待匹配响应。
// 步骤：读取端点代次 → 写入带命令标识的请求 → 等待响应并清理文件 → 返回结果或报告失败与超时。
Json send(
    const Path& directory, const std::string& command, const Json& arguments, std::string command_id, int timeout_ms) {
    const auto control = directory / "control";
    const auto endpoint = control / "endpoint.json";
    if (!std::filesystem::exists(endpoint))
        throw std::runtime_error("INSTANCE_NOT_ONLINE");

    const auto epoch = read(endpoint).at("epoch");
    const auto token = nonce();
    if (command_id.empty())
        command_id = token;

    const auto request = control / sdk::path_from_utf8(token + ".request.json");
    const auto response = control / sdk::path_from_utf8(token + ".response.json");
    write(request, {{"epoch", epoch}, {"command_id", command_id}, {"command", command}, {"arguments", arguments}});

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(response)) {
            const auto value = read(response);
            std::filesystem::remove(response);
            if (!value.at("ok").get<bool>())
                throw std::runtime_error(value.at("error").get<std::string>());
            return value.at("result");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("CONTROL_TIMEOUT: inspect state or retry with the same command ID");
}
}
