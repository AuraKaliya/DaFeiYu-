#include "search.hpp"

class Secondary : public dfy::examples::Search {
public:
    // 继承搜索提供者构造逻辑，按配置登记当前服务。
    using Search::Search;
    static constexpr auto id = "dafeiyu.search.secondary";
    static constexpr auto version = "0.5.0";
};
DFY_PLUGIN(Secondary)
