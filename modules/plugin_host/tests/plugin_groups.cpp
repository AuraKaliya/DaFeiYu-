#include <dafeiyu/host_services.hpp>
#include <dafeiyu/group_abi_v2.h>
#include <iostream>
#include <cstddef>
static_assert(sizeof(dfy_v2_group_member_info) == 16 && offsetof(dfy_v2_group_member_info,queued) == 8);
static_assert(sizeof(dfy_v2_group_api) == 32 && offsetof(dfy_v2_group_api,read) == 24);
namespace {
using namespace dfy::runtime;
using namespace dfy::events;
using namespace dfy::plugins;
// 在组授权、成员事务或实例隔离违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 查询精确版本服务，不读取未知布局。
// 步骤：查询版本 1 → 核对公共头 → 返回类型化表。
template<class T>
const T& service(std::string_view name) {
    const void* table = nullptr;
    check(HostServices::query({name.data(),name.size()},1,&table,nullptr) == DFY_V2_OK && table,"group API missing");
    const auto* header = static_cast<const dfy_v2_api_header*>(table);
    check(header->struct_size == sizeof(T) && header->interface_version == 1,"group API layout wrong");
    return *static_cast<const T*>(table);
}
// 创建空正文事件，成员失败不得改变其排队状态。
std::shared_ptr<const StructValue> payload() {
    const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.group",1,{}, {0,32}));
    return std::make_shared<const StructValue>(StructValue::create(type,{},0));
}
}
// 验证组 C 能力的条件授权、失败回滚、独立退出与关闭边界。
// 步骤：拒绝未授权上下文 → 加入及跨域拒绝 → 验证引用不足回滚 → 撤销授权并归还数据。
int main() {
    try {
        auto store = std::make_shared<EventStore>(4,0); auto group = std::make_shared<ConsumerGroup>(store,1,2);
        HostServices host(8), other(8); host.enable_group(group,1); other.enable_group(group,1);
        const auto event = group->publish(payload());
        auto binding = host.bind();
        const auto& groups = service<dfy_v2_group_api>(DFY_V2_GROUP_INTERFACE);
        const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
        const auto& reads = service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE);
        dfy_v2_handle receiver{},rejected{},request{}; dfy_v2_group_member_info info{};
        check(groups.join(&receiver,nullptr) == DFY_V2_OK && group->member_count() == 1 && host.resources() == 1 &&
            groups.read(receiver,&info,nullptr) == DFY_V2_OK && info.state == DFY_V2_GROUP_MEMBER_OPEN && info.queued == 1 && !info.close_mode,"group join or state wrong");
        check(refs.retain(receiver,&rejected,nullptr) == DFY_V2_UNSUPPORTED && !rejected.id &&
            groups.join(&rejected,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !rejected.id && group->member_count() == 1,"group reader budget bypassed");
        {
            auto scope = other.bind();
            check(groups.read(receiver,&info,nullptr) == DFY_V2_INVALID_HANDLE && !info.state && groups.leave(receiver,nullptr) == DFY_V2_INVALID_HANDLE,
                "other host controlled group member");
            check(groups.join(&rejected,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !rejected.id && !other.resources() && !other.references() && group->queued() == 1,
                "member capacity failure leaked or consumed data");
        }
        const auto wrong = host.expose(std::make_shared<const Value>(Value::from_bool(true)));
        check(groups.read(wrong,&info,nullptr) == DFY_V2_INVALID_ARGUMENT && !info.state && groups.leave(wrong,nullptr) == DFY_V2_INVALID_ARGUMENT &&
            groups.join(nullptr,nullptr) == DFY_V2_INVALID_ARGUMENT && groups.read(receiver,nullptr,nullptr) == DFY_V2_INVALID_ARGUMENT,"invalid group input accepted");
        refs.release(wrong,nullptr);
        const void* unavailable = &groups;
        check(HostServices::query({DFY_V2_GROUP_INTERFACE,sizeof(DFY_V2_GROUP_INTERFACE)-1},2,&unavailable,nullptr) == DFY_V2_UNSUPPORTED && !unavailable,"group version mismatch accepted");
        check(groups.leave(receiver,nullptr) == DFY_V2_OK && groups.leave(receiver,nullptr) == DFY_V2_OK && !group->member_count() && !host.resources() &&
            groups.read(receiver,&info,nullptr) == DFY_V2_OK && info.state == DFY_V2_GROUP_MEMBER_CLOSED && info.close_mode == DFY_V2_CLOSE_CANCEL && !info.queued && group->queued() == 1,
            "member leave closed shared group or changed backlog");
        refs.release(receiver,nullptr);
        {
            HostServices full(1); full.enable_group(group,1); auto scope = full.bind();
            const auto filler = full.expose(std::make_shared<const Value>(Value::null()));
            check(groups.join(&rejected,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !rejected.id && !group->member_count() && !full.resources() && full.references() == 1 && group->queued() == 1,
                "reference capacity failure left a member");
            refs.release(filler,nullptr);
            check(groups.join(&receiver,nullptr) == DFY_V2_OK && groups.leave(receiver,nullptr) == DFY_V2_OK,"group join did not recover after rollback");
            refs.release(receiver,nullptr);
        }
        {
            HostServices disabled(4); auto scope = disabled.bind(); unavailable = &groups;
            check(HostServices::query({DFY_V2_GROUP_INTERFACE,sizeof(DFY_V2_GROUP_INTERFACE)-1},1,&unavailable,nullptr) == DFY_V2_UNSUPPORTED && !unavailable &&
                groups.join(&rejected,nullptr) == DFY_V2_UNSUPPORTED && !rejected.id,"ungranted context joined group");
        }
        {
            HostServices orphan(4); orphan.enable_group(group,1); auto scope = orphan.bind();
            check(groups.join(&receiver,nullptr) == DFY_V2_OK && refs.release(receiver,nullptr) == DFY_V2_OK &&
                !orphan.references() && orphan.resources() == 1 && group->member_count() == 1,"client release abandoned member responsibility");
            check(orphan.begin_shutdown() && orphan.finish_shutdown() && !group->member_count() && group->queued() == 1,
                "host did not clean unreferenced member or changed backlog");
        }
        check(groups.join(&receiver,nullptr) == DFY_V2_OK && reads.next(receiver,&request,nullptr) == DFY_V2_OK,"group read submission failed");
        dfy_v2_read_result result{};
        check(reads.result(request,&result,nullptr) == DFY_V2_OK && result.outcome == DFY_V2_READ_EVENT && result.event_sequence == event.sequence,"rollback changed queued event");
        group->close(CloseMode::drain); host.advance();
        check(groups.read(receiver,&info,nullptr) == DFY_V2_OK && info.state == DFY_V2_GROUP_MEMBER_CLOSED && info.close_mode == DFY_V2_CLOSE_DRAIN &&
            !group->member_count() && groups.join(&rejected,nullptr) == DFY_V2_BUSY && !rejected.id,"closed group accepted new member");
        check(host.begin_shutdown() && host.finish_shutdown() && !host.references() && !store->references(),"group finalization leaked data");
        unavailable = &groups;
        check(HostServices::query({DFY_V2_GROUP_INTERFACE,sizeof(DFY_V2_GROUP_INTERFACE)-1},1,&unavailable,nullptr) == DFY_V2_UNSUPPORTED && !unavailable,"finished host retained group grant");
        std::cout << "C group membership checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
