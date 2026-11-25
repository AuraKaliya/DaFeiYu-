// 声明测试私有依赖的导出函数，强制生成真实 DLL 导入依赖。
__declspec(dllimport) int dfy_loader_dependency_value(void);

// 经真实依赖计算结果，证明加载过程完成了依赖解析。
__declspec(dllexport) int dfy_loader_value(void) { return dfy_loader_dependency_value() + 1; }

// 在真实 DLL 调用栈中回调宿主，返回后继续调用依赖以验证模块和依赖仍有效。
// 步骤：执行宿主回调 → 从依赖计算返回值。
__declspec(dllexport) int dfy_loader_invoke(void (*callback)(void*), void* context) {
    callback(context);
    return dfy_loader_dependency_value() + 1;
}
