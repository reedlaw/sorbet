#include "Types.h"
#include "common/sort.h"

using namespace std;
namespace sorbet::core {

// This sorts the underlying `origins`
vector<ErrorLine> TypeAndOrigins::origins2Explanations(const GlobalState &gs) const {
    vector<ErrorLine> result;
    auto compare = [](Loc left, Loc right) {
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
        result.emplace_back(o, "");
    }
    return result;
}

TypeAndOrigins::~TypeAndOrigins() noexcept {
    histogramInc("TypeAndOrigins.origins.size", origins.size());
}

} // namespace sorbet::core
