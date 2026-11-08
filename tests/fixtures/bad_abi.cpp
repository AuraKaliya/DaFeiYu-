#include <dafeiyu/abi.h>
extern "C" DFY_EXPORT int32_t DFY_CALL dfy_plugin_query(uint32_t, dfy_plugin_api* api) noexcept {
    api->abi_version = 999;
    return DFY_OK;
}
