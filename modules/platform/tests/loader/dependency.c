// 导出固定值，供直接加载及依赖搜索测试确认实际执行的 DLL。
__declspec(dllexport) int dfy_loader_dependency_value(void) { return 7; }
