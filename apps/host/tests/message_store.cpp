#include "message_store.hpp"
#include <winsqlite/winsqlite3.h>
#include <windows.h>
#include <iostream>

namespace {
using namespace dfy::app;
using Json = nlohmann::json;
// 通过另一个数据库连接制造真实写锁，解除时自动归还测试句柄。
struct Lock {
    sqlite3* db = nullptr;
    // 独占写入资格，同时允许原连接读取已经提交的数据。
    explicit Lock(const std::filesystem::path& profile) {
        const auto name = (profile/"main-conversation.sqlite3").u8string();
        if (sqlite3_open(reinterpret_cast<const char*>(name.c_str()),&db)!=SQLITE_OK ||
            sqlite3_exec(db,"BEGIN IMMEDIATE",nullptr,nullptr,nullptr)!=SQLITE_OK) throw std::runtime_error("fixture lock failed");
    }
    // 关闭未提交的事务，归还本测试的写锁。
    ~Lock() { sqlite3_close(db); }
};
// 在真实磁盘上核对入库、回执、重启及失败后的原号复用。
// 步骤：对比同号与同文 → 更新回执 → 阻塞独立写入 → 重开同库复查原事实。
void verify(const std::filesystem::path& profile) {
    const Json original{{"text","相同原话"},{"route","send"}}, pending{{"state","pending"}}, accepted{{"state","accepted"}};
    std::string conversation;
    {
        MessageStore store(profile);
        conversation = store.conversation();
        const auto first = store.put("desktop","a",original,pending);
        const auto same = store.put("desktop","a",original,pending);
        if (!first.created || same.created || same.message.id!=first.message.id || store.watermark()!=1)
            throw std::runtime_error("stable submission duplicated");
        try { (void)store.put("desktop","a",Json{{"text","不同"}},pending); throw std::logic_error("conflict accepted"); }
        catch (const std::invalid_argument&) {}
        const auto another = store.put("desktop","b",original,pending);
        if (!another.created || another.message.id==first.message.id) throw std::runtime_error("independent text suppressed");
        store.update(first.message.id,accepted);
        store.update(first.message.id,accepted);
        {
            Lock block(profile);
            try { (void)store.put("desktop","c",original,pending); throw std::logic_error("failed write acknowledged"); }
            catch (const std::runtime_error&) {}
            try { store.update(another.message.id,accepted); throw std::logic_error("locked receipt acknowledged"); }
            catch (const std::runtime_error&) {}
        }
        if (store.find("desktop","c") || store.watermark()!=3) throw std::runtime_error("failed write published");
    }
    MessageStore reopened(profile);
    const auto original_row = reopened.find("desktop","a");
    if (reopened.conversation()!=conversation || !original_row || original_row->receipt!=accepted || original_row->utc_ms<=0 ||
        reopened.find("desktop","b")->receipt!=pending || !reopened.put("desktop","c",original,pending).created)
        throw std::runtime_error("restart lost message or receipt");
}
}
// 在本工作树的独立目录运行，绝不读取生产对话文件。
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc!=2) return 2;
        verify(std::filesystem::path(argv[1])/std::to_string(GetCurrentProcessId())/std::to_string(GetTickCount64()));
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
