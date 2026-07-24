#include "window_filter.hpp"
namespace tsw {
bool isAltTabEligible(const RawWindow& w) {
    return w.visible && w.isRootOwner && !w.toolWindow && !w.cloaked && !w.title.empty();
}
}
