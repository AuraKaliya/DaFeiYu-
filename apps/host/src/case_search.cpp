#include "message_store_db.hpp"
#include <array>

namespace dfy::app::message_db {
// 只索引主人声明的对象和条件，语料中的重复原件不会为额外命中加分。
void index_case_terms(sqlite3* db,const std::string& name,const nlohmann::json& scope) {
    for(const std::string kind:{"name","object","measure","conditions"}) {
        Statement add(db,"INSERT OR IGNORE INTO case_term(name,kind,term) VALUES(?,?,?)");
        add.bind(1,name); add.bind(2,kind); add.bind(3,kind=="name" ? name : scope.at(kind).get_ref<const std::string&>()); add.next();
    }
}
// 对象、键或显式别名是定位依据，度量、适用条件与有效检查点仅参与消歧。
// 步骤：去重复问词 → 在仍有获准材料的组里归并字段命中 → 比较前两名 → 平分则返回歧义。
std::string resolve_case_terms(sqlite3* db,const std::vector<std::string>& topics,bool& ambiguous) {
    if (topics.size()==1) {
        Statement key(db,"SELECT c.name FROM observation_case c WHERE c.name=? AND EXISTS(SELECT 1 FROM case_member m WHERE m.name=c.name AND m.active=1)");
        key.bind(1,topics.front()); if(key.next()) return key.text(0);
    }
    Statement query(db,
        "WITH words(word) AS(VALUES(?1),(?2),(?3)),q(word) AS(SELECT DISTINCT lower(trim(word)) FROM words WHERE trim(word)<>''),"
        "hits(name,word,anchor,exact) AS("
        "SELECT t.name,q.word,t.kind IN('object','alias','name'),t.term=q.word FROM case_term t JOIN q "
        "WHERE instr(lower(t.term),q.word)>0 AND EXISTS(SELECT 1 FROM case_member m WHERE m.name=t.name AND m.active=1) "
        "UNION ALL SELECT m.name,q.word,0,1 FROM q JOIN case_member m ON m.checkpoint=q.word COLLATE NOCASE WHERE m.active=1 GROUP BY m.name,q.word),"
        "unique_hits(name,word,anchor,exact_anchor,exact) AS(SELECT name,word,max(anchor),max(anchor*exact),max(exact) FROM hits GROUP BY name,word) "
        "SELECT name,count(*) AS coverage,sum(exact_anchor) AS specific,sum(anchor) AS subjects,sum(exact) AS full FROM unique_hits "
        "GROUP BY name HAVING subjects>0 AND coverage=(SELECT count(*) FROM q) ORDER BY specific DESC,subjects DESC,full DESC,name LIMIT 2");
    for(std::size_t i=0;i<3;++i) query.bind(static_cast<int>(i+1),i<topics.size() ? topics[i] : std::string{});
    if(!query.next()) return {};
    const auto name=query.text(0); const auto score=[&]{return std::array{query.number(1),query.number(2),query.number(3),query.number(4)};};
    const auto best=score(); ambiguous=query.next() && score()==best;
    return ambiguous ? std::string{} : name;
}
}
