#include "resident_entry.hpp"
#include <vector>

// 将后台进程收到的宽字符路径交给本体管理入口。
int wmain(int argc, wchar_t** argv) {
    const std::vector<std::filesystem::path> args(argv + 1, argv + argc);
    return dfy::management::resident_command(args);
}
