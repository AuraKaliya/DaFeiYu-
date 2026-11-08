#pragma once
#include <dafeiyu/abi.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dfy::sdk {
using Json = nlohmann::json;
inline dfy_bytes bytes(std::string_view value) { return {value.data(), static_cast<uint64_t>(value.size())}; }
inline std::string text(dfy_bytes value) {
    if (value.size > 4 * 1024 * 1024 || (!value.data && value.size)) throw std::runtime_error("INVALID_ABI_BYTES");
    return value.size ? std::string(value.data, static_cast<size_t>(value.size)) : std::string{};
}
inline std::string path_text(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}
inline std::filesystem::path path_from_utf8(std::string_view text) {
    const std::u8string utf8(reinterpret_cast<const char8_t*>(text.data()), text.size());
    return std::filesystem::path(utf8);
}
class Cancelled : public std::runtime_error {
public: Cancelled() : std::runtime_error("CANCELLED") {}
};
class Call {
    const dfy_call& raw_;
public:
    std::string service, method;
    Json input, context;
    explicit Call(const dfy_call& raw) : raw_(raw), service(text(raw.service)), method(text(raw.method)),
        input(Json::parse(text(raw.input_json))), context(Json::parse(text(raw.context_json))) {}
    void checkpoint() const {
        if (raw_.is_cancelled && raw_.is_cancelled(raw_.cancellation_user)) throw Cancelled{};
    }
};
class Output {
    dfy_sink sink_;
public:
    explicit Output(dfy_sink sink) : sink_(sink) {}
    void emit(const Json& value) const {
        const auto encoded = value.dump();
        if (!sink_.emit || sink_.emit(sink_.user, bytes(encoded)) != DFY_OK) throw std::runtime_error("OUTPUT_REJECTED");
    }
};
class Host {
    dfy_host_api api_;
public:
    explicit Host(const dfy_host_api& api) : api_(api) {}
    std::filesystem::path data_directory() const { return path_from_utf8(text(api_.data_directory)); }
    std::string instance_id() const { return text(api_.instance_id); }
    void provide(std::string_view id) const {
        if (!api_.register_service || api_.register_service(api_.user, bytes(id)) != DFY_OK)
            throw std::runtime_error("SERVICE_REGISTRATION_REJECTED");
    }
};

// No C++ objects, exceptions or ownership transfers cross the ABI.
template<class Plugin> struct Export {
    template<class Fn> static int32_t guard(dfy_sink sink, Fn&& fn) noexcept {
        try { fn(); return DFY_OK; }
        catch (const Cancelled&) { return DFY_CANCELLED; }
        catch (const std::exception& e) { try { Output(sink).emit({{"error", e.what()}}); } catch (...) {} }
        catch (...) { try { Output(sink).emit({{"error", "PLUGIN_EXCEPTION"}}); } catch (...) {} }
        return DFY_ERROR;
    }
    static int32_t DFY_CALL create(const dfy_host_api* host, dfy_bytes config, void** instance, dfy_sink errors) noexcept {
        if (!host || !instance || host->struct_size < sizeof(dfy_host_api) || host->abi_version != DFY_ABI_VERSION) return DFY_ERROR;
        *instance = nullptr;
        return guard(errors, [&] { *instance = new Plugin(Host(*host), Json::parse(text(config))); });
    }
    static int32_t DFY_CALL start(void* instance, dfy_sink errors) noexcept {
        return guard(errors, [&] { static_cast<Plugin*>(instance)->start(); });
    }
    static int32_t DFY_CALL invoke(void* instance, const dfy_call* call, dfy_sink output) noexcept {
        if (!instance || !call || call->struct_size < sizeof(dfy_call)) return DFY_ERROR;
        return guard(output, [&] { Call request(*call); request.checkpoint(); static_cast<Plugin*>(instance)->invoke(request, Output(output)); });
    }
    static void DFY_CALL stop(void* instance) noexcept { try { static_cast<Plugin*>(instance)->stop(); } catch (...) {} }
    static void DFY_CALL destroy(void* instance) noexcept { delete static_cast<Plugin*>(instance); }
    static int32_t query(uint32_t abi, dfy_plugin_api* api) noexcept {
        if (abi != DFY_ABI_VERSION || !api || api->struct_size < sizeof(dfy_plugin_api)) return DFY_ERROR;
        *api = {sizeof(dfy_plugin_api), DFY_ABI_VERSION, Plugin::id, Plugin::version, create, start, invoke, stop, destroy};
        return DFY_OK;
    }
};
}
#define DFY_PLUGIN(PluginType) \
    extern "C" DFY_EXPORT int32_t DFY_CALL dfy_plugin_query(uint32_t abi, dfy_plugin_api* api) noexcept { \
        return ::dfy::sdk::Export<PluginType>::query(abi, api); \
    }
