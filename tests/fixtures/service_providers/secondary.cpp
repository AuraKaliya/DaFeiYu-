#include "search.hpp"
class Secondary : public dfy::examples::Search {
public:
    using Search::Search;
    static constexpr auto id = "dafeiyu.search.secondary";
    static constexpr auto version = "0.5.0";
};
DFY_PLUGIN(Secondary)
