#pragma once
#include <dafeiyu/abi.h>
#include <dafeiyu/value.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dfy::sdk {
// 借用字符串内存构造 ABI 字节视图，调用期间由原字符串持有数据。
inline dfy_bytes bytes(std::string_view value) {
    return {value.data(), value.size()};
}

// 校验 ABI 字节范围并复制为宿主持有的字符串。
inline std::string text(dfy_bytes value) {
    if (value.size > 4 * 1024 * 1024 || (!value.data && value.size))
        throw std::runtime_error("INVALID_ABI_BYTES");
    return value.size ? std::string(value.data, static_cast<size_t>(value.size)) : std::string{};
}

struct Call {
    uint64_t id;
    std::string service, method;
    Json input, context;

    // 从插件调用事件中提取调用标识、服务、方法、输入及上下文。
    explicit Call(const Json& event)
        : id(event.at("call_id")), service(event.at("service")), method(event.at("method")), input(event.at("input")),
          context(event.at("context")) {}
};

struct ActionInvocation {
    std::string action_id, operation_id;
    int definition_revision;
    Json arguments;

    // 解析动作调用包装，并校验操作标识与宿主上下文一致。
    explicit ActionInvocation(const Call& call)
        : action_id(call.input.at("action_id")), operation_id(call.input.at("operation_id")),
          definition_revision(call.input.at("definition_revision")), arguments(call.input.at("arguments")) {
        if (operation_id != call.context.at("operation_id").get<std::string>() || operation_id.empty())
            throw std::runtime_error("ACTION_OPERATION_MISMATCH");
    }
};

class Host {
    dfy_host_api api_;

public:
    // 保存插件可调用的宿主 ABI 接口表。
    explicit Host(const dfy_host_api& api) : api_(api) {}

    // 取得宿主分配给当前插件的独立数据目录。
    std::filesystem::path data_directory() const { return path_from_utf8(text(api_.data_directory)); }

    // 读取当前插件所属的实例标识。
    std::string instance_id() const { return text(api_.instance_id); }

    // 将服务或动作声明编码后提交宿主注册，失败时抛出异常。
    void register_item(std::string_view kind, const Json& value) const {
        const auto encoded = value.dump();
        if (api_.register_item(api_.user, bytes(kind), bytes(encoded)) != DFY_OK)
            throw std::runtime_error("REGISTRATION_REJECTED");
    }

    // 向宿主登记清单中声明的服务。
    void provide(std::string_view service) const { register_item("service", service); }

    // 向宿主登记启动阶段发现的动作定义。
    void action(const Json& descriptor) const { register_item("action", descriptor); }

    // 序列化并提交宿主消息，返回入队状态而非业务执行结果。
    int post(const Json& message) const {
        const auto encoded = message.dump();
        return api_.post(api_.user, bytes(encoded));
    }

    // 提交宿主消息，并将队列繁忙或关闭状态转换为异常。
    void send(const Json& message) const {
        const int status = post(message);
        if (status != DFY_OK)
            throw std::runtime_error(status == DFY_BUSY ? "HOST_BUSY" : "HOST_CLOSED");
    }

    // 查询宿主是否已取消调用或认定调用超过期限。
    bool cancelled(uint64_t call) const { return api_.is_cancelled(api_.user, call) != 0; }

    // 向宿主提交服务调用的成功完成消息。
    void complete(uint64_t call, const Json& result) const {
        send({{"type", "complete"}, {"call_id", call}, {"result", result}});
    }

    // 向宿主提交服务调用的错误完成消息。
    void fail(uint64_t call, const Json& failure) const {
        send({{"type", "complete"}, {"call_id", call}, {"error", failure}});
    }

    // 尽力发送调用进度，忽略进度消息入队失败。
    void progress(uint64_t call, const Json& value) const {
        (void)post({{"type", "progress"}, {"call_id", call}, {"value", value}});
    }

    // 请求宿主在父调用的权限和期限内调用依赖服务。
    void request(uint64_t parent, std::string key, std::string binding, std::string method, const Json& input,
        int timeout_ms = 0) const {
        Json message{{"type", "request_service"}, {"call_id", parent}, {"key", key}, {"binding", binding},
            {"method", method}, {"input", input}};

        if (timeout_ms > 0)
            message["timeout_ms"] = timeout_ms;
        send(message);
    }

    // 以指定渠道和去重键向宿主提交外部输入。
    void input(std::string channel, std::string key, const Json& value) const {
        send({{"type", "post_input"}, {"channel", channel}, {"key", key}, {"input", value}});
    }

    // 请求宿主为父动作调用登记可恢复的后台作业。
    void register_job(uint64_t parent, std::string key, std::string work_key) const {
        send({{"type", "register_job"}, {"call_id", parent}, {"key", key}, {"work_key", work_key}});
    }

    // 向宿主提交带序号的后台作业终态结果。
    void job_result(std::string job, uint64_t sequence, const Json& value) const {
        send({{"type", "job_result"}, {"job_id", job}, {"sequence", sequence}, {"result", value}});
    }

    // 请求宿主在实例或父调用边界内创建作用域。
    void open_scope(std::string key, std::string kind, uint64_t parent = 0) const {
        send({{"type", "open_scope"}, {"key", key}, {"kind", kind}, {"call_id", parent}});
    }

    // 请求宿主关闭指定作用域及其关联资源。
    void close_scope(std::string key, std::string scope) const {
        send({{"type", "close_scope"}, {"key", key}, {"scope_id", scope}});
    }

    // 请求在作用域内登记上下文、动作可见性或订阅资源。
    void register_scoped(std::string scope, std::string key, std::string kind, const Json& value) const {
        send({{"type", "register_scoped"}, {"scope_id", scope}, {"key", key}, {"kind", kind}, {"value", value}});
    }

    // 请求宿主释放指定作用域资源注册。
    void release(std::string key, std::string registration) const {
        send({{"type", "release_registration"}, {"key", key}, {"registration_id", registration}});
    }

    // 请求宿主在有效作用域内发起可选限时的只读服务调用。
    void request_scoped(std::string scope, std::string key, std::string binding, std::string method, const Json& input,
        int timeout_ms = 0) const {
        Json message{{"type", "request_scoped"}, {"scope_id", scope}, {"key", key}, {"binding", binding},
            {"method", method}, {"input", input}};

        if (timeout_ms > 0)
            message["timeout_ms"] = timeout_ms;
        send(message);
    }

    // 请求按事件类型及过滤条件订阅作用域内的事实日志。
    void subscribe(std::string scope, std::string key, const Json& types, const Json& filter = Json::object()) const {
        send({{"type", "subscribe"}, {"scope_id", scope}, {"key", key}, {"types", types}, {"filter", filter}});
    }

    // 向宿主确认已消费的事实事件页游标。
    void acknowledge(std::string registration, uint64_t sequence) const {
        send({{"type", "ack_facts"}, {"registration_id", registration}, {"sequence", sequence}});
    }

    // 请求读取作用域允许公开的状态视图。
    void read_scoped(std::string scope, std::string key, std::string object) const {
        send({{"type", "read_scoped"}, {"scope_id", scope}, {"key", key}, {"object", object}});
    }

    // 请求宿主在父操作关联下执行获准的变更命令。
    void command(uint64_t parent, std::string key, std::string name, const Json& arguments) const {
        send({{"type", "submit_command"}, {"call_id", parent}, {"key", key}, {"command", name},
            {"arguments", arguments}});
    }

    // 查询父操作中指定命令键对应的持久化回执。
    void command_receipt(uint64_t parent, std::string key) const {
        send({{"type", "query_command"}, {"call_id", parent}, {"key", key}});
    }

    // 请求读取父调用所属任务或实例行为的公开状态。
    void read_state(uint64_t parent, std::string key, std::string object) const {
        send({{"type", "read_state"}, {"call_id", parent}, {"key", key}, {"object", object}});
    }
};

class Plugin {
protected:
    Host host;

public:
    // 保存插件生命周期和服务处理所需的宿主访问器。
    explicit Plugin(Host value) : host(value) {}

    // 提供默认空启动钩子，由需要启动工作的插件覆盖。
    void start() {}

    // 提供默认空停用钩子，由需要停止接收工作的插件覆盖。
    void quiesce() {}

    // 提供默认空停止钩子，由需要回收资源的插件覆盖。
    void stop() {}
};

template <class T> struct Export {
    // 在 ABI 边界捕获插件异常，并通过错误输出回调报告。
    // 步骤：执行受保护操作 → 将标准或未知异常编码为错误 → 返回 ABI 成功或失败状态。
    template <class Fn> static int32_t guard(dfy_sink errors, Fn&& fn) noexcept {
        try {
            fn();
            return DFY_OK;
        } catch (const std::exception& e) {
            try {
                const auto data = error(e.what()).dump();
                errors.emit(errors.user, bytes(data));
            } catch (...) {
            }
        } catch (...) {
            try {
                const auto data = error("PLUGIN_EXCEPTION").dump();
                errors.emit(errors.user, bytes(data));
            } catch (...) {
            }
        }
        return DFY_ERROR;
    }

    // 校验宿主 ABI 并创建具体插件对象，避免异常穿越 C 接口。
    // 步骤：校验接口版本及结构大小 → 清空输出对象 → 在异常保护内解析配置并构造插件。
    static int32_t DFY_CALL create(const dfy_host_api* api, dfy_bytes config, void** object, dfy_sink errors) noexcept {
        if (!api || !object || api->abi_version != DFY_ABI_VERSION || api->struct_size != sizeof(dfy_host_api))
            return DFY_ERROR;

        *object = nullptr;
        return guard(errors, [&] { *object = new T(Host(*api), Json::parse(text(config))); });
    }

    // 在异常保护内调用具体插件的启动钩子。
    static int32_t DFY_CALL start(void* object, dfy_sink errors) noexcept {
        return guard(errors, [&] { static_cast<T*>(object)->start(); });
    }

    // 在异常保护内解析宿主事件并交给插件处理。
    static int32_t DFY_CALL event(void* object, dfy_bytes value, dfy_sink errors) noexcept {
        return guard(errors, [&] { static_cast<T*>(object)->on_event(Json::parse(text(value))); });
    }

    // 调用插件停用钩子，并在 ABI 边界抑制异常。
    static void DFY_CALL quiesce(void* object) noexcept {
        try {
            static_cast<T*>(object)->quiesce();
        } catch (...) {
        }
    }

    // 调用插件停止钩子，并在 ABI 边界抑制异常。
    static void DFY_CALL stop(void* object) noexcept {
        try {
            static_cast<T*>(object)->stop();
        } catch (...) {
        }
    }

    // 销毁具体插件对象并释放其内存。
    static void DFY_CALL destroy(void* object) noexcept { delete static_cast<T*>(object); }

    // 校验 ABI 查询参数，并填充插件身份和生命周期入口表。
    static int32_t query(uint32_t abi, dfy_plugin_api* api) noexcept {
        if (abi != DFY_ABI_VERSION || !api || api->struct_size != sizeof(dfy_plugin_api))
            return DFY_ERROR;

        *api = {
            sizeof(dfy_plugin_api), DFY_ABI_VERSION, T::id, T::version, create, start, event, quiesce, stop, destroy};
        return DFY_OK;
    }
};
}

// 导出具体插件类型的 ABI 查询入口，供宿主动态加载。
#define DFY_PLUGIN(Type)                                                                                               \
    extern "C" DFY_EXPORT int32_t DFY_CALL dfy_plugin_query(uint32_t abi, dfy_plugin_api* api) noexcept {              \
        return ::dfy::sdk::Export<Type>::query(abi, api);                                                              \
    }
