#pragma once
#include <dafeiyu/value.hpp>
#include <memory>

namespace dfy {
using Json = sdk::Json;
using Path = std::filesystem::path;

struct Profile {
    Json data;
    Path plugins;
    std::string foundation;
    // 加载并校验实例配置，解析插件目录和基础提示词。
    static Profile open(const Path& file);
};

// 命令可以并发提交；关闭和析构由所有者串行执行。
// 实例持有唯一的核心状态写线程。
class InstanceRuntime {
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    // 启动实例核心线程，并等待组件初始化完成。
    InstanceRuntime(Profile profile, Path state);
    // 关闭实例并等待核心线程退出。
    ~InstanceRuntime();
    // 禁止复制或赋值，保持当前对象独占的资源与状态所有权。
    InstanceRuntime(const InstanceRuntime&) = delete;
    // 禁止复制或赋值，保持当前对象独占的资源与状态所有权。
    InstanceRuntime& operator=(const InstanceRuntime&) = delete;
    // 线程安全地提交宿主命令，并等待核心线程返回结果。
    Json request(const std::string& command, const Json& arguments = Json::object());
    // 标记实例关闭并唤醒核心线程，随后等待其退出。
    void shutdown();
};
}
