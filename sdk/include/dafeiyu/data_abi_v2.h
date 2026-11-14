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
#define DFY_V2_VALUE_STRUCTURE UINT32_C(7)
#define DFY_V2_VALUE_NULL UINT32_C(8)
#define DFY_V2_VALUE_ARRAY UINT32_C(9)
#define DFY_V2_VALUE_MAP UINT32_C(10)
#define DFY_V2_VALUE_ENUM UINT32_C(11)
#define DFY_V2_VALUE_UNION UINT32_C(12)
#define DFY_V2_UNION_INTERFACE "dfy.unions"
#define DFY_V2_UNION_VERSION UINT32_C(1)
#define DFY_V2_ENUM_INTERFACE "dfy.enums"
#define DFY_V2_ENUM_VERSION UINT32_C(1)
#define DFY_V2_CONSTRAINT_INTERFACE "dfy.constraints"
#define DFY_V2_CONSTRAINT_VERSION UINT32_C(1)
#define DFY_V2_CONSTRAINT_VERSION_2 UINT32_C(2)
#define DFY_V2_MAP_INTERFACE "dfy.maps"
#define DFY_V2_MAP_VERSION UINT32_C(1)
#define DFY_V2_ARRAY_INTERFACE "dfy.arrays"
#define DFY_V2_ARRAY_VERSION UINT32_C(1)
#define DFY_V2_ELEMENT_NULLABLE UINT32_C(1)
#define DFY_V2_FIELD_OPTIONAL UINT32_C(1)
#define DFY_V2_FIELD_NULLABLE UINT32_C(2)
#define DFY_V2_TYPE_VERSION_2 UINT32_C(2)
#define DFY_V2_VALUE_VERSION_2 UINT32_C(2)
#define DFY_V2_STRUCT_VERSION_2 UINT32_C(2)
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
// 能力版本 2 的 kind 为可扩展标记，未知标记须明确拒绝，不得按标量解释。
typedef struct dfy_v2_field_info_v2 {
    dfy_v2_string_view name;
    uint32_t kind;
    uint32_t flags;
    uint64_t max_bytes;
} dfy_v2_field_info_v2;
// flags 是可扩展约束标记；可选与可空相互独立，未知标志必须明确拒绝。
typedef struct dfy_v2_type_api_v2 {
    dfy_v2_api_header header;
    // 与版本 1 相同的类型元数据和借用期。
    dfy_v2_status (DFY_V2_CALL *describe)(dfy_v2_handle type, dfy_v2_type_info* output, dfy_v2_error* error);
    // 返回字段分类及基础约束，允许结构分类，复杂约束通过对应访问能力取得。
    dfy_v2_status (DFY_V2_CALL *field)(dfy_v2_handle type, uint64_t index, dfy_v2_field_info_v2* output, dfy_v2_error* error);
    // 为结构字段的描述取得独立引用，非结构字段拒绝，输出必需且失败清零。
    dfy_v2_status (DFY_V2_CALL *nested)(dfy_v2_handle type, uint64_t index, dfy_v2_handle* output, dfy_v2_error* error);
} dfy_v2_type_api_v2;
typedef struct dfy_v2_value_api_v2 {
    dfy_v2_api_header header;
    // 基础值同版本 1；结构、容器、枚举、联合与 null 的 data 为零，内容通过专用能力访问。
    dfy_v2_status (DFY_V2_CALL *read)(dfy_v2_handle value, dfy_v2_value_view* output, dfy_v2_error* error);
    // 取得结构内容的独立持有，保持原值及其预算链，非结构值拒绝。
    dfy_v2_status (DFY_V2_CALL *structure)(dfy_v2_handle value, dfy_v2_handle* output, dfy_v2_error* error);
} dfy_v2_value_api_v2;
// 版本 2 的 field 可返回组合或 null；已声明但缺失为 NOT_FOUND，越界为 INVALID_ARGUMENT。
typedef dfy_v2_struct_api dfy_v2_struct_api_v2;
// 版本 1 的元素仅为基础种类，版本 2 可返回组合分类；element_flags 仅支持 ELEMENT_NULLABLE。
typedef struct dfy_v2_array_type_info {
    uint32_t element_kind;
    uint32_t element_flags;
    uint64_t max_elements;
    uint64_t max_element_bytes;
} dfy_v2_array_type_info;
typedef struct dfy_v2_array_info { dfy_v2_array_type_info type; uint64_t size; } dfy_v2_array_info;
typedef struct dfy_v2_array_api {
    dfy_v2_api_header header;
    // 复制结构字段声明的数组约束，不产生引用；非数组字段拒绝。
    dfy_v2_status (DFY_V2_CALL *field_type)(dfy_v2_handle type, uint64_t index, dfy_v2_array_type_info* output, dfy_v2_error* error);
    // 复制实际数组的约束及元素数，空数组仍是数组值。
    dfy_v2_status (DFY_V2_CALL *describe)(dfy_v2_handle value, dfy_v2_array_info* output, dfy_v2_error* error);
    // 取得元素的独立持有，保持父数组及原始预算；越界不是缺失，返回 INVALID_ARGUMENT。
    dfy_v2_status (DFY_V2_CALL *element)(dfy_v2_handle value, uint64_t index, dfy_v2_handle* output, dfy_v2_error* error);
} dfy_v2_array_api;
// 映射键为 UTF-8 字符串，value_flags 当前仅允许 ELEMENT_NULLABLE。
typedef struct dfy_v2_map_type_info {
    uint32_t value_kind;
    uint32_t value_flags;
    uint64_t max_entries;
    uint64_t max_key_bytes;
    uint64_t max_value_bytes;
} dfy_v2_map_type_info;
typedef struct dfy_v2_map_info { dfy_v2_map_type_info type; uint64_t size; } dfy_v2_map_info;
typedef struct dfy_v2_map_api {
    dfy_v2_api_header header;
    // 复制结构字段的映射约束，非映射字段拒绝。
    dfy_v2_status (DFY_V2_CALL *field_type)(dfy_v2_handle type, uint64_t index, dfy_v2_map_type_info* output, dfy_v2_error* error);
    // 复制实际映射的约束与条目数。
    dfy_v2_status (DFY_V2_CALL *describe)(dfy_v2_handle value, dfy_v2_map_info* output, dfy_v2_error* error);
    // 按字节序返回借用键与独立值引用；键依赖传入映射引用，两个输出均必需且失败清空。
    dfy_v2_status (DFY_V2_CALL *entry)(dfy_v2_handle value, uint64_t index, dfy_v2_string_view* key, dfy_v2_handle* output, dfy_v2_error* error);
    // 按完整键精确查询并返回独立值引用，未知键为 NOT_FOUND，空键合法。
    dfy_v2_status (DFY_V2_CALL *find)(dfy_v2_handle value, dfy_v2_string_view key, dfy_v2_handle* output, dfy_v2_error* error);
} dfy_v2_map_api;
// 闭合枚举身份；underlying_kind 仅为 INT64 或 UINT64，名称借用依赖描述引用。
typedef struct dfy_v2_enum_info {
    dfy_v2_string_view name;
    uint32_t contract_version;
    uint32_t underlying_kind;
    uint64_t member_count;
} dfy_v2_enum_info;
// 成员名依赖传入描述引用；number 为带精确整数种类的副本，reserved 固定为零。
typedef struct dfy_v2_enum_member { dfy_v2_string_view name; dfy_v2_value_view number; } dfy_v2_enum_member;
// 索引属于本描述的声明顺序，稳定整数在 number 中，二者不能互换。
typedef struct dfy_v2_enum_selection { uint64_t index; dfy_v2_value_view number; } dfy_v2_enum_selection;
typedef struct dfy_v2_enum_api {
    dfy_v2_api_header header;
    // 同宿主线程绑定中取得枚举字段描述的独立持有；所有输出必需，失败全零，error 可空。
    dfy_v2_status (DFY_V2_CALL *field_type)(dfy_v2_handle type, uint64_t index, dfy_v2_handle* output, dfy_v2_error* error);
    // 读取枚举元数据；非枚举描述拒绝，名称借用至传入引用释放。
    dfy_v2_status (DFY_V2_CALL *describe)(dfy_v2_handle type, dfy_v2_enum_info* output, dfy_v2_error* error);
    // 按声明顺序读取成员；越界返回 INVALID_ARGUMENT，不截断或转换整数。
    dfy_v2_status (DFY_V2_CALL *member)(dfy_v2_handle type, uint64_t index, dfy_v2_enum_member* output, dfy_v2_error* error);
    // 从枚举值取得描述独立持有；描述不保留数据及其事件预算，调用方负责释放。
    dfy_v2_status (DFY_V2_CALL *descriptor)(dfy_v2_handle value, dfy_v2_handle* output, dfy_v2_error* error);
    // 复制枚举值选中的成员索引及整数；值引用仍承担原始数据预算，非枚举值拒绝。
    dfy_v2_status (DFY_V2_CALL *read)(dfy_v2_handle value, dfy_v2_enum_selection* output, dfy_v2_error* error);
} dfy_v2_enum_api;
// 独立内容约束引用复用字段元数据，名称借用依赖该约束引用，不保持事件正文。
typedef struct dfy_v2_constraint_api {
    dfy_v2_api_header header;
    // 同宿主线程绑定中取得结构字段约束的独立持有；输出必需，失败全零，error 可空。
    dfy_v2_status (DFY_V2_CALL *field)(dfy_v2_handle type, uint64_t index, dfy_v2_handle* output, dfy_v2_error* error);
    // 读取种类、名称、可选／可空及字节约束，借用期依赖传入约束引用。
    dfy_v2_status (DFY_V2_CALL *describe)(dfy_v2_handle constraint, dfy_v2_field_info_v2* output, dfy_v2_error* error);
    // 为结构、枚举或联合返回对应种类的独立描述引用；其他种类拒绝，调用方负责释放。
    dfy_v2_status (DFY_V2_CALL *named_type)(dfy_v2_handle constraint, dfy_v2_handle* output, dfy_v2_error* error);
    // 复制标量数组约束，组合元素返回 UNSUPPORTED，非数组内容返回 INVALID_ARGUMENT。
    dfy_v2_status (DFY_V2_CALL *array)(dfy_v2_handle constraint, dfy_v2_array_type_info* output, dfy_v2_error* error);
    // 复制标量映射约束，组合值返回 UNSUPPORTED，非映射内容返回 INVALID_ARGUMENT。
    dfy_v2_status (DFY_V2_CALL *map)(dfy_v2_handle constraint, dfy_v2_map_type_info* output, dfy_v2_error* error);
} dfy_v2_constraint_api;
// 版本 2 支持完整容器元素约束；匿名元素的 name 为空，optional 固定为 false。
typedef struct dfy_v2_constraint_api_v2 {
    dfy_v2_api_header header;
    // 同版本 1，取得结构字段约束的独立持有，失败清空输出。
    dfy_v2_status (DFY_V2_CALL *field)(dfy_v2_handle type, uint64_t index, dfy_v2_handle* output, dfy_v2_error* error);
    // 读取完整分类和标志，名称借用依赖传入约束引用；所有输出必需，error 可空。
    dfy_v2_status (DFY_V2_CALL *describe)(dfy_v2_handle constraint, dfy_v2_field_info_v2* output, dfy_v2_error* error);
    // 按真实种类返回命名描述持有，标量及容器自身没有命名类型。
    dfy_v2_status (DFY_V2_CALL *named_type)(dfy_v2_handle constraint, dfy_v2_handle* output, dfy_v2_error* error);
    // 复制数组数量及元素基本约束，组合元素通过 element 取得完整定义。
    dfy_v2_status (DFY_V2_CALL *array)(dfy_v2_handle constraint, dfy_v2_array_type_info* output, dfy_v2_error* error);
    // 复制映射的键、值和容量约束，完整值定义通过 element 取得。
    dfy_v2_status (DFY_V2_CALL *map)(dfy_v2_handle constraint, dfy_v2_map_type_info* output, dfy_v2_error* error);
    // 同宿主线程绑定内取得数组元素或映射值的匿名约束持有；只保持描述，不占事件预算。
    dfy_v2_status (DFY_V2_CALL *element)(dfy_v2_handle constraint, dfy_v2_handle* output, dfy_v2_error* error);
} dfy_v2_constraint_api_v2;
// 联合名称借用依赖描述引用，版本和分支数为副本，reserved 固定为零。
typedef struct dfy_v2_union_info {
    dfy_v2_string_view name;
    uint32_t contract_version;
    uint32_t reserved;
    uint64_t branch_count;
} dfy_v2_union_info;
// 索引按声明顺序，标签是完整 UInt64，零与最大值均合法。
typedef struct dfy_v2_union_selection { uint64_t index; uint64_t tag; } dfy_v2_union_selection;
typedef struct dfy_v2_union_api {
    dfy_v2_api_header header;
    // 同宿主线程绑定中读取联合描述；所有输出必需，失败全零，error 可空。
    dfy_v2_status (DFY_V2_CALL *describe)(dfy_v2_handle type, dfy_v2_union_info* output, dfy_v2_error* error);
    // 返回分支标签及独立约束引用，由 constraints 反射；任一失败清空两个输出。
    dfy_v2_status (DFY_V2_CALL *branch)(dfy_v2_handle type, uint64_t index, uint64_t* tag, dfy_v2_handle* constraint, dfy_v2_error* error);
    // 为联合值取得独立描述持有，不保留事件数据预算，调用方负责释放。
    dfy_v2_status (DFY_V2_CALL *descriptor)(dfy_v2_handle value, dfy_v2_handle* output, dfy_v2_error* error);
    // 复制实际选择的分支索引及标签，不读取未选中内容。
    dfy_v2_status (DFY_V2_CALL *read)(dfy_v2_handle value, dfy_v2_union_selection* output, dfy_v2_error* error);
    // 为实际内容取得独立持有，保持联合及原事件预算；带标签 null 仍返回有效值引用。
    dfy_v2_status (DFY_V2_CALL *content)(dfy_v2_handle value, dfy_v2_handle* output, dfy_v2_error* error);
} dfy_v2_union_api;
#pragma pack(pop)
#ifdef __cplusplus
}
#endif
