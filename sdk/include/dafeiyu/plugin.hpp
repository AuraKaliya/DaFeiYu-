#pragma once
#include <dafeiyu/abi.h>
#include <dafeiyu/value.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dfy::sdk {
inline dfy_bytes bytes(std::string_view value) { return {value.data(), value.size()}; }
inline std::string text(dfy_bytes value) {
    if (value.size > 4 * 1024 * 1024 || (!value.data && value.size)) throw std::runtime_error("INVALID_ABI_BYTES");
    return value.size ? std::string(value.data, static_cast<size_t>(value.size)) : std::string{};
}
struct Call {
    uint64_t id;
    std::string service, method;
    Json input, context;
    explicit Call(const Json& event) : id(event.at("call_id")), service(event.at("service")), method(event.at("method")),
        input(event.at("input")), context(event.at("context")) {}
};
class Host {
    dfy_host_api api_;
public:
    explicit Host(const dfy_host_api& api) : api_(api) {}
    std::filesystem::path data_directory() const { return path_from_utf8(text(api_.data_directory)); }
    std::string instance_id() const { return text(api_.instance_id); }
    void register_item(std::string_view kind, const Json& value) const {
        const auto encoded = value.dump();
        if (api_.register_item(api_.user, bytes(kind), bytes(encoded)) != DFY_OK) throw std::runtime_error("REGISTRATION_REJECTED");
    }
    void provide(std::string_view service) const { register_item("service", service); }
    void action(const Json& descriptor) const { register_item("action", descriptor); }
    int post(const Json& message) const { const auto encoded = message.dump(); return api_.post(api_.user, bytes(encoded)); }
    void send(const Json& message) const {
        const int status = post(message);
        if (status != DFY_OK) throw std::runtime_error(status == DFY_BUSY ? "HOST_BUSY" : "HOST_CLOSED");
    }
    bool cancelled(uint64_t call) const { return api_.is_cancelled(api_.user, call) != 0; }
    void complete(uint64_t call, const Json& result) const { send({{"type", "complete"}, {"call_id", call}, {"result", result}}); }
    void fail(uint64_t call, const Json& failure) const { send({{"type", "complete"}, {"call_id", call}, {"error", failure}}); }
    void progress(uint64_t call, const Json& value) const { (void)post({{"type", "progress"}, {"call_id", call}, {"value", value}}); }
    void request(uint64_t parent, std::string key, std::string binding, std::string method, const Json& input, int timeout_ms = 0) const {
        Json message{{"type", "request_service"}, {"call_id", parent}, {"key", key}, {"binding", binding}, {"method", method}, {"input", input}};
        if (timeout_ms > 0) message["timeout_ms"] = timeout_ms;
        send(message);
    }
    void input(std::string channel, std::string key, const Json& value) const { send({{"type", "post_input"}, {"channel", channel}, {"key", key}, {"input", value}}); }
    void register_job(uint64_t parent, std::string key, std::string work_key) const {
        send({{"type", "register_job"}, {"call_id", parent}, {"key", key}, {"work_key", work_key}});
    }
    void job_result(std::string job, uint64_t sequence, const Json& value) const {
        send({{"type", "job_result"}, {"job_id", job}, {"sequence", sequence}, {"result", value}});
    }
    void subscribe(std::string key, const Json& types) const { send({{"type", "subscribe"}, {"key", key}, {"types", types}}); }
    void acknowledge(std::string key, uint64_t sequence) const { send({{"type", "ack_facts"}, {"key", key}, {"sequence", sequence}}); }
    void command(uint64_t parent, std::string key, std::string name, const Json& arguments) const {
        send({{"type", "submit_command"}, {"call_id", parent}, {"key", key}, {"command", name}, {"arguments", arguments}});
    }
    void command_receipt(uint64_t parent, std::string key) const { send({{"type", "query_command"}, {"call_id", parent}, {"key", key}}); }
    void read_state(uint64_t parent, std::string key, std::string object) const { send({{"type", "read_state"}, {"call_id", parent}, {"key", key}, {"object", object}}); }
};
class Plugin {
protected:
    Host host;
public:
    explicit Plugin(Host value) : host(value) {}
    void start() {}
    void quiesce() {}
    void stop() {}
};
template<class T> struct Export {
    template<class Fn> static int32_t guard(dfy_sink errors, Fn&& fn) noexcept {
        try { fn(); return DFY_OK; }
        catch (const std::exception& e) { try { const auto data = error(e.what()).dump(); errors.emit(errors.user, bytes(data)); } catch (...) {} }
        catch (...) { try { const auto data = error("PLUGIN_EXCEPTION").dump(); errors.emit(errors.user, bytes(data)); } catch (...) {} }
        return DFY_ERROR;
    }
    static int32_t DFY_CALL create(const dfy_host_api* api, dfy_bytes config, void** object, dfy_sink errors) noexcept {
        if (!api || !object || api->abi_version != DFY_ABI_VERSION || api->struct_size != sizeof(dfy_host_api)) return DFY_ERROR;
        *object = nullptr;
        return guard(errors, [&] { *object = new T(Host(*api), Json::parse(text(config))); });
    }
    static int32_t DFY_CALL start(void* object, dfy_sink errors) noexcept { return guard(errors, [&] { static_cast<T*>(object)->start(); }); }
    static int32_t DFY_CALL event(void* object, dfy_bytes value, dfy_sink errors) noexcept {
        return guard(errors, [&] { static_cast<T*>(object)->on_event(Json::parse(text(value))); });
    }
    static void DFY_CALL quiesce(void* object) noexcept { try { static_cast<T*>(object)->quiesce(); } catch (...) {} }
    static void DFY_CALL stop(void* object) noexcept { try { static_cast<T*>(object)->stop(); } catch (...) {} }
    static void DFY_CALL destroy(void* object) noexcept { delete static_cast<T*>(object); }
    static int32_t query(uint32_t abi, dfy_plugin_api* api) noexcept {
        if (abi != DFY_ABI_VERSION || !api || api->struct_size != sizeof(dfy_plugin_api)) return DFY_ERROR;
        *api = {sizeof(dfy_plugin_api), DFY_ABI_VERSION, T::id, T::version, create, start, event, quiesce, stop, destroy};
        return DFY_OK;
    }
};
}
#define DFY_PLUGIN(Type) extern "C" DFY_EXPORT int32_t DFY_CALL dfy_plugin_query(uint32_t abi, dfy_plugin_api* api) noexcept { return ::dfy::sdk::Export<Type>::query(abi, api); }
