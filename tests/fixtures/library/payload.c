// 声明测试私有依赖的导出函数，强制生成真实 DLL 导入依赖。
__declspec(dllimport) int dfy_loader_dependency_value(void);

// 经真实依赖计算结果，证明加载过程完成了依赖解析。
__declspec(dllexport) int dfy_loader_value(void) { return dfy_loader_dependency_value() + 1; }
