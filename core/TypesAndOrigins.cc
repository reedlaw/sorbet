#include "Types.h"
#include "common/sort.h"

using namespace std;
namespace sorbet::core {

// This sorts the underlying `origins`
vector<ErrorLine> TypeAndOrigins::origins2Explanations(const GlobalState &gs, const Loc &locForUninitialized) const {
    vector<ErrorLine> result;

    std::cout << "location we are assuming for uninitialized:" << std::endl
              << locForUninitialized.toString(gs) << std::endl;

    auto compare = [&locForUninitialized](Loc left, Loc right) {
        if (left == locForUninitialized && right != locForUninitialized) {
            return false;
        }
        if (left != locForUninitialized && right == locForUninitialized) {
            return true;
        }

        if (left.file() != right.file()) {
            return left.file().id() < right.file().id();
        }
        if (left.beginPos() != right.beginPos()) {
            return left.beginPos() < right.beginPos();
        }
        if (left.endPos() != right.endPos()) {
            return left.endPos() < right.endPos();
        }
        return false;
    };
    auto sortedOrigins = origins;
    fast_sort(sortedOrigins, compare);
    Loc last;
    for (auto o : sortedOrigins) {
        if (o == last) {
            continue;
        }
        last = o;

        if (o == locForUninitialized) {
            result.emplace_back(ErrorLine::from(
                o, "Type may be `{}` since it depends on variables that are not necessarily initialized here:",
                "NilClass"));
        } else {
            result.emplace_back(o, "");
        }
    }
    return result;
}

TypeAndOrigins::~TypeAndOrigins() noexcept {
    histogramInc("TypeAndOrigins.origins.size", origins.size());
}

} // namespace sorbet::core
