#include "search.hpp"
class Primary : public dfy::examples::Search {
public:
    using Search::Search;
    static constexpr auto id = "dafeiyu.search.primary";
    static constexpr auto version = "0.4.0";
};
DFY_PLUGIN(Primary)
