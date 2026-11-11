#include <dafeiyu/runtime_ids.hpp>
#include "id_sequence.hpp"

namespace dfy::runtime {
namespace {
std::atomic<std::uint64_t> last_domain{0};
}

// 为宿主中新建的签发域分配独立身份，销毁宿主对象不会重置计数器。
DomainId next_domain() { return detail::allocate_id(last_domain); }
}
