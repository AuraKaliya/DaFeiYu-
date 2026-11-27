#pragma once
#include "message_store_db.hpp"
#include <vector>

namespace dfy::app::case_review {
// 从已接纳的轻量索引挑选见证；只为实际要交给模型的引用展开正文。
struct Member {
    std::int64_t id,record;
    std::string ref,checkpoint,kind,path,reader,reading,attribution;
};
struct Evidence {
    std::string state;
    std::size_t total=0,natives=0,paths=0;
    std::int64_t latest=0;
    std::vector<Member> members;
    std::vector<std::string> conflict;
};
// 核对截止成员以前各条路径的可比性并优先选出矛盾双方。
Evidence classify(sqlite3* db,const std::string& name,const nlohmann::json& scope,std::int64_t through);
// 从优先见证和其余原件取一页，不加载略过的原件正文。
std::vector<Member> page(sqlite3* db,const std::string& name,std::int64_t through,const Evidence& evidence,std::size_t offset,std::size_t count);
// 读取一份选中材料的原文片段、来源及稳定编号。
nlohmann::json fact(sqlite3* db,const Member& row);
}
