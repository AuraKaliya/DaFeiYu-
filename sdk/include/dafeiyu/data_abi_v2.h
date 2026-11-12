#pragma once
#include <dafeiyu/plugin_abi_v2.h>
#define DFY_V2_TYPE_INTERFACE "dfy.types"
#define DFY_V2_TYPE_VERSION UINT32_C(1)
#define DFY_V2_VALUE_INTERFACE "dfy.values"
#define DFY_V2_VALUE_VERSION UINT32_C(1)
#define DFY_V2_STRUCT_INTERFACE "dfy.structures"
#define DFY_V2_STRUCT_VERSION UINT32_C(1)
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
// 种类由外层 kind 指定，只读取对应成员；布尔固定使用 0 或 1。
typedef union dfy_v2_value_data {
    uint32_t boolean;
    int64_t int64;
    uint64_t uint64;
    double float64;
    dfy_v2_string_view string;
    dfy_v2_bytes_view bytes;
} dfy_v2_value_data;
// 数值为副本，文本和字节仅借用至传入值引用释放；reserved 固定为零。
typedef struct dfy_v2_value_view {
    uint32_t kind;
    uint32_t reserved;
    dfy_v2_value_data data;
} dfy_v2_value_view;
typedef struct dfy_v2_value_api {
    dfy_v2_api_header header;
    // 精确读取基础值，不隐式转换；输出必需且失败全零，error 可空。
    dfy_v2_status (DFY_V2_CALL *read)(dfy_v2_handle value, dfy_v2_value_view* output, dfy_v2_error* error);
} dfy_v2_value_api;
typedef struct dfy_v2_struct_api {
    dfy_v2_api_header header;
    // 取得结构描述的独立持有引用，成功后由调用方释放，失败清空输出。
    dfy_v2_status (DFY_V2_CALL *descriptor)(dfy_v2_handle value, dfy_v2_handle* output, dfy_v2_error* error);
    // 按描述顺序取得基础字段的独立持有，字段可以比原结构引用存活更久。
    dfy_v2_status (DFY_V2_CALL *field)(dfy_v2_handle value, uint64_t index, dfy_v2_handle* output, dfy_v2_error* error);
} dfy_v2_struct_api;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
