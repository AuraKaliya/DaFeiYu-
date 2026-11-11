#pragma once
#include <cstdint>

// 宿主只链接一份 dfy_runtime；插件通过宿主服务取得句柄，不自行链接和签发域。
namespace dfy::runtime {
using DomainId = std::uint64_t;

// 在宿主内签发进程期不复用的非零域；耗尽抛出 overflow_error，不发布关联对象。
DomainId next_domain();
}
