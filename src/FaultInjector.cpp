// Singleton storage for FaultInjector::instance().
//
// Defined in a translation unit (rather than as a function-local static in
// the header) so that the static library and any consumer share one and
// only one instance. With a header-only static-local, linkers can produce
// duplicate copies — the engine then arms point A while the test reads
// counter B and the chaos signal silently disappears.

#include "FaultInjector.h"

namespace OrderMatcher {

FaultInjector& FaultInjector::instance() {
    static FaultInjector self;
    return self;
}

}  // namespace OrderMatcher
