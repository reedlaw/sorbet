#include "Types.h"
#include "common/sort.h"

using namespace std;
namespace sorbet::core {

// This sorts the underlying `origins`
vector<ErrorLine> TypeAndOrigins::origins2Explanations(const GlobalState &gs) const {
    vector<ErrorLine> result;
    auto compare = [](TypeOrigin left, TypeOrigin right) {
        // If "nil because uninitialized" is present, the list will read best if it goes last.
        if (!left.becauseUninitialized && right.becauseUninitialized) {
            return true;
        }
        if (left.becauseUninitialized && !right.becauseUninitialized) {
            return false;
        }

        if (left.loc.file() != right.loc.file()) {
            return left.loc.file().id() < right.loc.file().id();
        }
        if (left.loc.beginPos() != right.loc.beginPos()) {
            return left.loc.beginPos() < right.loc.beginPos();
        }
        if (left.loc.endPos() != right.loc.endPos()) {
            return left.loc.endPos() < right.loc.endPos();
        }
        return false;
    };
    auto sortedOrigins = origins;
    fast_sort(sortedOrigins, compare);
    Loc last;
    for (auto o : sortedOrigins) {
        if (o.loc == last) {
            continue;
        }
        last = o.loc;
        ErrorLine errorLine =
            o.becauseUninitialized
                ? ErrorLine::from(o.loc, "Variable may be `{}`, because it is not necessarily initialized here:", "nil")
                : ErrorLine(o.loc, "");
        result.emplace_back(errorLine);
    }
    return result;
}

TypeAndOrigins::~TypeAndOrigins() noexcept {
    histogramInc("TypeAndOrigins.origins.size", origins.size());
}

} // namespace sorbet::core
