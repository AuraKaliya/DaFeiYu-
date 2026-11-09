#include "control.hpp"
#include <chrono>
#include <fstream>
#include <random>
#include <thread>
namespace dfy::host {
namespace {
std::string nonce() {
    std::random_device source;
    std::string result; constexpr char digits[] = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) result += digits[source() & 15];
    return result;
}
Json read(const Path& file) {
    if (std::filesystem::file_size(file) > 1024 * 1024) throw std::runtime_error("CONTROL_MESSAGE_TOO_LARGE");
    std::ifstream stream(file, std::ios::binary); if (!stream) throw std::runtime_error("CONTROL_READ");
    return Json::parse(stream);
}
void write(const Path& target, const Json& value) {
    auto temporary = target; temporary += ".tmp-" + nonce();
    { std::ofstream stream(temporary, std::ios::binary); stream << value.dump(); stream.flush(); if (!stream) throw std::runtime_error("CONTROL_WRITE"); }
    std::filesystem::rename(temporary, target);
}
}
void serve(InstanceRuntime& runtime, const Path& directory, const std::atomic<bool>& interrupted) {
    const auto control = directory / "control"; std::filesystem::create_directories(control);
    const auto epoch = nonce();
    const auto endpoint = control / "endpoint.json";
    if (std::filesystem::exists(endpoint)) std::filesystem::remove(endpoint);
    write(endpoint, {{"epoch", epoch}, {"protocol", 1}});
    bool stopping = false;
    while (!interrupted.load() && !stopping) {
        for (const auto& entry : std::filesystem::directory_iterator(control)) {
            const auto name = sdk::path_text(entry.path().filename());
            if (!entry.is_regular_file() || !name.ends_with(".request.json")) continue;
            Json response;
            try {
                const auto request = read(entry.path());
                if (request.at("epoch") != epoch) throw std::runtime_error("CONTROL_EPOCH_EXPIRED");
                auto args = request.at("arguments"); args["_command_id"] = request.at("command_id");
                const auto command = request.at("command").get<std::string>();
                response = {{"ok", true}, {"result", runtime.request(command, args)}};
                if (command == "host.shutdown") stopping = true;
            } catch (const std::exception& error) { response = {{"ok", false}, {"error", error.what()}}; }
            const auto response_path = control / sdk::path_from_utf8(name.substr(0, name.size() - 13) + ".response.json");
            if (!std::filesystem::exists(response_path)) write(response_path, response);
            std::filesystem::remove(entry.path());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::filesystem::remove(endpoint);
    runtime.shutdown();
}
Json send(const Path& directory, const std::string& command, const Json& arguments, std::string command_id, int timeout_ms) {
    const auto control = directory / "control";
    const auto endpoint = control / "endpoint.json";
    if (!std::filesystem::exists(endpoint)) throw std::runtime_error("INSTANCE_NOT_ONLINE");
    const auto epoch = read(endpoint).at("epoch"); const auto token = nonce();
    if (command_id.empty()) command_id = token;
    const auto request = control / sdk::path_from_utf8(token + ".request.json");
    const auto response = control / sdk::path_from_utf8(token + ".response.json");
    write(request, {{"epoch", epoch}, {"command_id", command_id}, {"command", command}, {"arguments", arguments}});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::filesystem::exists(response)) {
            const auto value = read(response); std::filesystem::remove(response);
            if (!value.at("ok").get<bool>()) throw std::runtime_error(value.at("error").get<std::string>());
            return value.at("result");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("CONTROL_TIMEOUT: inspect state or retry with the same command ID");
}
}
