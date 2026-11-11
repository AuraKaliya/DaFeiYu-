#include <stddef.h>
#include <string.h>
#pragma pack(push, 1)
#include "api.h"
struct borrow_packing_probe { char tag; uint64_t value; };
#pragma pack(pop)
_Static_assert(sizeof(struct borrow_packing_probe) == 9, "packing restored");
_Static_assert(sizeof(dfy_v2_handle) == 24 && _Alignof(dfy_v2_handle) == 8 &&
    offsetof(dfy_v2_handle, id) == 8 && offsetof(dfy_v2_handle, generation) == 16, "handle layout");
_Static_assert(sizeof(dfy_v2_bytes_view) == 16 && _Alignof(dfy_v2_bytes_view) == 8 &&
    offsetof(dfy_v2_bytes_view, length) == 8, "bytes layout");
_Static_assert(sizeof(dfy_v2_reference_api) == 24 && _Alignof(dfy_v2_reference_api) == 8 &&
    offsetof(dfy_v2_reference_api, retain) == 8 && offsetof(dfy_v2_reference_api, release) == 16, "reference layout");
_Static_assert(sizeof(dfy_test_borrow_api) == 16 && offsetof(dfy_test_borrow_api, preserve) == 8, "fixture layout");

// 读取宿主借用数据并返回独立引用，DLL 不保存服务指针或承担数据释放逻辑。
// 步骤：清空输出并校验服务 → 读取含零字节的数据 → 保留引用并转交调用方。
static dfy_v2_status DFY_V2_CALL preserve(const dfy_v2_reference_api* refs, dfy_test_view_fn view,
    dfy_v2_handle source, dfy_v2_handle* owned, dfy_v2_error* error) {
    const dfy_v2_handle empty = {0};
    dfy_v2_bytes_view bytes = {0};
    dfy_v2_status status;
    if (owned) *owned = empty;
    if (error) error->length = 0;
    if (!owned || !refs || !view) return DFY_V2_INVALID_ARGUMENT;
    if (refs->header.struct_size != sizeof(*refs) || refs->header.interface_version != DFY_V2_REFERENCE_VERSION)
        return DFY_V2_UNSUPPORTED;
    if (!refs->retain || !refs->release) return DFY_V2_INVALID_ARGUMENT;

    status = view(source, &bytes);
    if (status != DFY_V2_OK) return status;
    if (bytes.length != 3 || !bytes.data || memcmp(bytes.data, "A\0B", 3) != 0) return DFY_V2_INVALID_ARGUMENT;
    return refs->retain(source, owned, error);
}
const dfy_test_borrow_api dfy_test_borrow = {{sizeof(dfy_test_borrow_api), 1}, preserve};
