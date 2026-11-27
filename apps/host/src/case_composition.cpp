#include "message_store_db.hpp"
#include <algorithm>
#include <array>

namespace dfy::app::message_db {
namespace {
// 只移除首尾的输入空格，保留标识内部所有分隔符。
std::string_view trimmed(std::string_view s) {
    const auto first=s.find_first_not_of(" ");
    return first==s.npos ? std::string_view{} : s.substr(first,s.find_last_not_of(" ")-first+1);
}
// 将全角 ASCII、全角空格及 ASCII 大小写映射到同一表示，保留其余 UTF-8 字节。
std::string folded(std::string_view input) {
    std::string out; out.reserve(input.size());
    while(!input.empty()) {
        auto c=static_cast<unsigned char>(input.front()); input.remove_prefix(1);
        if(c==0xe3 && input.starts_with("\x80\x80")) {c=' '; input.remove_prefix(2);}
        else if(c==0xef && input.size()>=2) {
            const auto b=static_cast<unsigned char>(input[0]), d=static_cast<unsigned char>(input[1]);
            if((b==0xbc && d>=0x81 && d<=0xbf) || (b==0xbd && d>=0x80 && d<=0x9e)) {
                c=static_cast<unsigned char>(b==0xbc ? d-0x60 : d-0x20); input.remove_prefix(2);
            }
        }
        out.push_back(static_cast<char>(c>='A' && c<='Z' ? c+('a'-'A') : c));
    }
    return std::string(trimmed(out));
}
// 子串可以命中对象的一部分，但不能从连续编号中截掉首位或末位。
bool number_match(std::string_view term,std::string_view word) {
    const auto digit=[](char c){return c>='0' && c<='9';};
    if(word.empty()) return false;
    for(auto i=term.find(word);i!=term.npos;i=term.find(word,i+1)) {
        const auto end=i+word.size();
        if((!i || !digit(word.front()) || !digit(term[i-1])) && (end==term.size() || !digit(word.back()) || !digit(term[end]))) return true;
    }
    return false;
}
// 借用 SQLite 的定长文本参数，仅在当前回调期间有效。
std::string_view input_text(sqlite3_value* v) {
    const auto* p=reinterpret_cast<const char*>(sqlite3_value_text(v));
    return p ? std::string_view(p,static_cast<std::size_t>(sqlite3_value_bytes(v))) : std::string_view{};
}
struct Field {std::string kind,text;};
// 条件、度量和当前仍可读的采样检查点必须整项匹配。
bool qualifier(sqlite3* db,const std::string& name,const std::vector<Field>& fields,std::string_view word) {
    if(std::ranges::any_of(fields,[&](const auto& f){return (f.kind=="conditions" || f.kind=="measure") && f.text==word;})) return true;
    Statement q(db,"SELECT 1 FROM case_member WHERE name=? AND active=1 AND checkpoint=? COLLATE NOCASE LIMIT 1");
    q.bind(1,name); q.bind(2,word); return q.next();
}
// 从问词一端取完整登记名称，余下部分只能是同组条件，或在登记对象中出现的别名限定词。
bool composed(sqlite3* db,const std::string& name,const std::vector<Field>& fields,const Field& anchor,std::string_view word) {
    if(anchor.kind=="name" || anchor.text.empty() || word.size()<=anchor.text.size()) return false;
    for(bool suffix:{true,false}) {
        auto part=word;
        bool separated=false;
        if(suffix && part.ends_with(anchor.text)) {
            part=part.substr(0,part.size()-anchor.text.size()); separated=part.ends_with(' '); part=trimmed(part);
            if(part.ends_with("的")) {part.remove_suffix(std::string_view("的").size()); separated=true;}
        } else if(!suffix && part.starts_with(anchor.text)) {
            part=part.substr(anchor.text.size()); separated=part.starts_with(' '); part=trimmed(part);
            if(part.starts_with("的")) {part.remove_prefix(std::string_view("的").size()); separated=true;}
        } else continue;
        part=trimmed(part); if(part.empty()) continue;
        if(separated && qualifier(db,name,fields,part)) return true;
        // 别名前已有的非 ASCII 限定片段必须也是登记对象的前缀，纯编号不能充当位置。
        if(suffix && anchor.kind=="alias" && part.size()>=6 &&
            std::ranges::all_of(part,[](unsigned char c){return c>=0x80;}) &&
            std::ranges::any_of(fields,[&](const auto& f){return f.kind=="object" && f.text.starts_with(part);})) return true;
    }
    return false;
}
// 每个问词须在同一组得到解释，其中至少一个须指向完整对象、别名或比较键。
int support_level(sqlite3* db,const std::string& name,const std::vector<std::string>& words,bool whole_only) {
    Statement q(db,"SELECT kind,term FROM case_term WHERE name=?"); q.bind(1,name);
    std::vector<Field> fields; while(q.next()) fields.push_back({q.text(0),folded(q.text(1))});
    int level=0;
    for(const auto& word:words) {
        const bool matched=std::ranges::any_of(fields,[&](const auto& f){return (f.kind=="object" || f.kind=="alias" || f.kind=="name") &&
            (f.text==word || (!whole_only && composed(db,name,fields,f,word)));});
        if(!matched && !qualifier(db,name,fields,word)) return 0;
        if(matched && !level) level=1;
    }
    if(level && std::ranges::any_of(fields,[&](const auto& f){return (f.kind=="object" || f.kind=="alias" || f.kind=="name") && f.text==words.front();})) return 2;
    return level;
}
}
// 注册只读查询所用的确定性标量函数，避免按字节截断 UTF-8 或把相邻编号混同。
void register_case_matching(sqlite3* db) {
    const auto flags=SQLITE_UTF8|SQLITE_DETERMINISTIC;
    check(sqlite3_create_function_v2(db,"dfy_case_fold",1,flags,nullptr,[](sqlite3_context* ctx,int,sqlite3_value** v){
        try {const auto s=folded(input_text(v[0])); sqlite3_result_text(ctx,s.data(),static_cast<int>(s.size()),SQLITE_TRANSIENT);}
        catch(const std::bad_alloc&) {sqlite3_result_error_nomem(ctx);}
    },nullptr,nullptr,nullptr));
    check(sqlite3_create_function_v2(db,"dfy_number_match",2,flags,nullptr,[](sqlite3_context* ctx,int,sqlite3_value** v){
        try {sqlite3_result_int(ctx,number_match(folded(input_text(v[0])),folded(input_text(v[1]))));}
        catch(const std::bad_alloc&) {sqlite3_result_error_nomem(ctx);}
    },nullptr,nullptr,nullptr));
}
// 词面查回不确定时核对完整登记名与组合，保留同优先级的歧义。
// 步骤：折叠查询字宽 → 找到包含完整对象的可见组 → 逐项核对其余线索。
CaseTermMatch resolve_composed_terms(sqlite3* db,const std::vector<std::string>& topics,bool whole_only) {
    std::vector<std::string> words; std::string joined;
    for(const auto& topic:topics) {
        auto w=folded(topic); if(w.empty() || w.find('/')!=w.npos) return {};
        joined+=w+"\n"; words.push_back(std::move(w));
    }
    Statement candidates(db,"SELECT t.name,dfy_case_fold(t.term),EXISTS(SELECT 1 FROM case_member m WHERE m.name=t.name AND m.active=1) "
        "FROM case_term t WHERE t.kind IN('object','alias','name') AND instr(?,dfy_case_fold(t.term))>0 ORDER BY t.name");
    candidates.bind(1,joined);
    std::array<CaseTermMatch,3> matches; bool registered=false; std::string last;
    while(candidates.next()) {
        const auto name=candidates.text(0);
        registered|=candidates.text(1)==words.front();
        if(last==name || !candidates.number(2)) continue;
        last=name; const auto level=support_level(db,name,words,whole_only);
        if(!level) continue;
        auto& match=matches[level];
        if(match.name.empty()) match.name=name; else match.ambiguous=true;
    }
    // 已登记的整名即使撤权也不改指旁机；只向上层交付可见组或无命中。
    auto result=matches[registered ? 2 : 1]; result.registered=registered;
    if(result.ambiguous) result.name.clear();
    return result;
}
}
