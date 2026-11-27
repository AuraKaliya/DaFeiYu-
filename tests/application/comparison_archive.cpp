#include "task_memory_fixture.hpp"
#include "main_messages.hpp"
#include "message_store_db.hpp"
#include "case_review.hpp"

namespace dfy::tests::task_memory {
using namespace app::message_db;
// 比例档案只合成无外部副作用的原件；索引回填和别名维护使用正式实现。
// 步骤：写入第十三版可见与已撤原件 → 重开升级 → 登记及移除主人别名。
void populate_comparison_archive(const fs::path& data,unsigned groups) {
    const auto profile=app::MessageStore(data).conversation();
    sqlite3* db=nullptr; const auto file=(data/"main-conversation.sqlite3").u8string(); check(sqlite3_open(reinterpret_cast<const char*>(file.c_str()),&db));
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> close(db,&sqlite3_close);
    Transaction tx(db); sql(db,"DROP TABLE case_term; PRAGMA user_version=13; UPDATE conversation SET memory_revision=1");
    for(unsigned g=1;g<=groups;++g) {
        const std::string name=g==1 ? "east" : g==2 ? "west" : g==3 ? "night" : g==4 ? "withdrawn" : "cabinet-"+std::to_string(g);
        const std::string obj=g==1 || g==3 ? "东馆交接台" : g==2 ? "西馆交接台" : g==4 ? "已撤红标对象" : "东馆资料柜"+std::to_string(g);
        const std::string condition=g==1 ? "早班交接后" : g==2 ? "午班交接后" : g==3 ? "晚班复核" : "例行巡查";
        const std::string checkpoint=g==1 ? "FirstShift" : g==2 ? "中午点" : g==3 ? "收馆点" : "巡查点";
        const Json scope{{"object",obj},{"measure","交接是否达到预期"},{"unit","布尔"},{"conditions",condition},{"criterion","target"}};
        Statement group(db,"INSERT INTO observation_case(name,scope) VALUES(?,?)"); group.bind(1,name); group.bind(2,scope.dump()); group.next();
        for(unsigned n=1;n<=(g<=3 ? 2u : 1u);++n) {
            const auto id=g*10+n; const std::string tool=n==1 ? "registry/set" : "display/set", reader=n==1 ? "registry/read" : "display/read";
            const auto path=Json::array({tool,reader}).dump(), base=profile+"/task/"+std::to_string(id); const bool met=g!=1 || n!=2;
            Statement task(db,"INSERT INTO task_memory(id,origin,goal,tool,body,reason,signature,active) VALUES(?,?,?,?,'{}','','',?)");
            task.bind(1,id); task.bind(2,base); task.bind(3,obj); task.bind(4,tool); task.bind(5,g!=4); task.next();
            if(g!=4) {
                Statement obs(db,"INSERT INTO task_observation VALUES(?,?,'off','on',?,2,1,3,'applied')");
                obs.bind(1,id); obs.bind(2,path); obs.bind(3,met ? "on" : "off"); obs.next();
            }
            Statement member(db,"INSERT INTO case_member(name,checkpoint,reference,kind,record,dependency,attribution,path,reader,reading,met,active) "
                "VALUES(?,?,?,'tool_readback',?,?,'at_start',?,?,?,?,?)");
            member.bind(1,name); member.bind(2,checkpoint); member.bind(3,base+"/receipt/3"); member.bind(4,id); member.bind(5,base);
            member.bind(6,path); member.bind(7,reader); member.bind(8,met ? "met" : "missed"); member.bind(9,met); member.bind(10,g!=4); member.next();
        }
    }
    tx.commit(); close.reset();
    app::Application host({},data/"empty",data,model::Backend{[](const auto&,const auto&)->model::Response{throw std::runtime_error("unexpected alias model request");}});
    host.bind_messages(std::make_shared<app::MessageStore>(data),"scope-admin",nullptr);
    auto* owner=host.main_messages();
    owner->manage_case({"/comparison","alias","east","东边签收台"}); owner->manage_case({"/comparison","alias","east","东边签收台"});
    owner->manage_case({"/comparison","alias","west","西翼交班桌"}); owner->manage_case({"/comparison","alias","night","夜间交班桌"});
    owner->manage_case({"/comparison","alias","east","已移除别名"}); owner->manage_case({"/comparison","unalias","east","已移除别名"});
}
// 一组连续观测中第一项与第二项相斥，第三项与这两项分别共用读者或读数。
// 步骤：产生有稳定原号的读回 → 插入已收到的独立报告 → 计量宿主实际持有的候选行数。
std::size_t populate_long_archive(const fs::path& data,unsigned natives) {
    const auto profile=app::MessageStore(data).conversation();
    sqlite3* db=nullptr; const auto file=(data/"main-conversation.sqlite3").u8string();
    check(sqlite3_open(reinterpret_cast<const char*>(file.c_str()),&db)); std::unique_ptr<sqlite3,decltype(&sqlite3_close)> close(db,&sqlite3_close);
    Transaction tx(db);
    const Json scope{{"object","长年台账"},{"measure","达标"},{"conditions","登记后"},{"unit","布尔"},{"criterion","target"}};
    Statement group(db,"INSERT INTO observation_case(name,scope) VALUES('ledger',?)"); group.bind(1,scope.dump()); group.next();
    for(unsigned i=1;i<=natives;++i) {
        const std::string path=i==2 ? "bar" : "foo", reader=path+"/read", action=path+"/set";
        const auto family=Json::array({action,reader}).dump(),base=profile+"/task/"+std::to_string(i);
        const auto checkpoint=i<=3 ? std::string{"early"} : std::to_string(i);
        Statement t(db,"INSERT INTO task_memory(id,origin,goal,tool,body,reason,signature) VALUES(?,?,'长期登记',?,'{}','','')");
        t.bind(1,i); t.bind(2,base); t.bind(3,action); t.next();
        Statement o(db,"INSERT INTO task_observation VALUES(?,?,'off','on',?,2,1,3,'applied')");
        o.bind(1,i); o.bind(2,family); o.bind(3,i==1 ? "off" : "on"); o.next();
        Statement m(db,"INSERT INTO case_member(name,checkpoint,reference,kind,record,dependency,attribution,path,reader,reading,met) "
            "VALUES('ledger',?,?,'tool_readback',?,?,'at_start',?,?,?,?)");
        m.bind(1,checkpoint); m.bind(2,base+"/receipt/3"); m.bind(3,i); m.bind(4,base); m.bind(5,family);
        m.bind(6,reader); m.bind(7,i==1 ? "missed" : "met"); m.bind(8,i!=1); m.next();
    }
    sql(db,"INSERT INTO child_owner(id,plugin,preset,subject,channel,access) VALUES(7,'example',2,'业务组','',3)");
    const Json envelope{{"author",profile+"/child/7"},{"outcome","succeeded"},{"summary","交接班转述仍需现场确认。"},{"summaryKind","sender_summary"}};
    Statement letter(db,"INSERT INTO child_letter(id,owner,exchange,phase,signature,envelope,source,received) VALUES(1,7,'交接','end','fixture',?,0,1)");
    letter.bind(1,envelope.dump()); letter.next();
    Statement m(db,"INSERT INTO case_member(name,checkpoint,reference,kind,record,dependency,attribution,path,reader,reading,met) "
        "VALUES('ledger','early',?,'sender_report',1,?,'owner_retrospective','','','',0)");
    const auto ref=profile+"/letter/1"; m.bind(1,ref); m.bind(2,ref); m.next();
    index_case_terms(db,"ledger",scope); sql(db,"UPDATE conversation SET memory_revision=1"); tx.commit();
    return app::case_review::classify(db,"ledger",scope,INT64_MAX).members.size();
}
}
