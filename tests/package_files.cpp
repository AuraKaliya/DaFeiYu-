#include "../plugins/runtime/package_files.hpp"
#include <fstream>
#include <iostream>
namespace {
namespace fs = std::filesystem;
namespace files = dfy::plugins::files;
// 在包目录边界、容量或清理安全性违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 要求路径操作拒绝并保留无关测试失败。
template<class Action>
void rejects(Action action) { bool rejected = false; try { action(); } catch (const std::exception&) { rejected = true; } check(rejected,"unsafe package operation accepted"); }
// 写入小型测试文件，不覆盖测试工作目录外的路径。
void write(const fs::path& file, const char* text) { std::ofstream output(file); output << text; output.close(); check(static_cast<bool>(output),"package test write failed"); }
}
// 验证本地普通目录、真实链接拒绝、有界树及移动删除前的实际边界检查。
// 步骤：建立唯一工作区 → 检查树和容量 → 拒绝外部移动与删除 → 私有发布及清理 → 拒绝真实链接。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2,"package filesystem work path required"); fs::create_directories(argv[1]); fs::path run;
        for (unsigned i = 1;; ++i) { run = fs::path(argv[1])/std::to_string(i); if (fs::create_directory(run)) break; }
        const auto root = files::directory(run/u8"插件 根",true); const auto outside = files::directory(run/"outside",true); write(outside/"keep.txt","keep");
        auto stage = files::temporary(root,".install-"); fs::create_directory(stage/"nested"); write(stage/"nested"/"entry.dll","test");
        check(files::inspect(stage).size() == 2,"package tree inspection lost nodes");
        rejects([&] { (void)files::inspect(stage,{1,64,4}); }); rejects([&] { (void)files::inspect(stage,{4,3,4}); });
        rejects([&] { (void)files::inspect(stage,{4,64,1}); });
        rejects([&] { files::move(root,stage,outside/"escaped"); }); rejects([&] { files::erase(root,outside); });
        check(fs::exists(stage/"nested"/"entry.dll") && fs::exists(outside/"keep.txt"),"boundary rejection changed files");
        files::move(root,stage,root/"published"); rejects([&] { files::erase(root,root/"published"); });
        auto trash = files::temporary(root,".removed-"); fs::remove(trash); files::move(root,root/"published",trash); files::erase(root,trash);
        check(!fs::exists(trash) && fs::exists(outside/"keep.txt"),"private cleanup escaped root or left files");
        stage = files::temporary(root,".install-"); fs::create_symlink(outside/"keep.txt",stage/"linked.txt");
        rejects([&] { (void)files::inspect(stage); }); rejects([&] { files::erase(root,stage); });
        check(fs::exists(outside/"keep.txt"),"link rejection removed external target"); fs::remove(stage/"linked.txt"); files::erase(root,stage);
        fs::create_directory_symlink(outside,root/"junction"); rejects([&] { (void)files::directory(root/"junction"); });
        fs::remove(root/"junction"); std::cout << "Package filesystem checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
