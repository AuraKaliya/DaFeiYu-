#pragma once
#include <dafeiyu/plugin_abi_v2.h>
#define DFY_V2_TYPE_INTERFACE "dfy.types"
#define DFY_V2_TYPE_VERSION UINT32_C(1)
#define DFY_V2_VALUE_BOOL UINT32_C(1)
#define DFY_V2_VALUE_INT64 UINT32_C(2)
#define DFY_V2_VALUE_UINT64 UINT32_C(3)
#define DFY_V2_VALUE_FLOAT64 UINT32_C(4)
#define DFY_V2_VALUE_STRING UINT32_C(5)
#define DFY_V2_VALUE_BYTES UINT32_C(6)
#ifdef __cplusplus
extern "C" {
#endif
#pragma pack(push, 8)
// 名称借用至传入类型引用释放，版本和字段数为副本，reserved 固定为零。
typedef struct dfy_v2_type_info {
    dfy_v2_string_view name;
    uint32_t contract_version;
    uint32_t reserved;
    uint64_t field_count;
} dfy_v2_type_info;
// 字段名称依赖传入类型引用；字段必填且不可空，数值 max_bytes 为零。
typedef struct dfy_v2_field_info {
    dfy_v2_string_view name;
    uint32_t kind;
    uint32_t reserved;
    uint64_t max_bytes;
} dfy_v2_field_info;
typedef struct dfy_v2_type_api {
    dfy_v2_api_header header;
    // 在宿主绑定中读取结构描述，输出必需且失败全零；不隐式保留来源。
    dfy_v2_status (DFY_V2_CALL *describe)(dfy_v2_handle type, dfy_v2_type_info* output, dfy_v2_error* error);
    // 按声明顺序读取字段，越界或对象种类不符返回 INVALID_ARGUMENT；error 可空。
    dfy_v2_status (DFY_V2_CALL *field)(dfy_v2_handle type, uint64_t index, dfy_v2_field_info* output, dfy_v2_error* error);
} dfy_v2_type_api;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
