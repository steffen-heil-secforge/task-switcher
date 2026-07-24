#pragma once
#include <string>
namespace tsw {
struct RawWindow { bool visible=false, isRootOwner=false, toolWindow=false, cloaked=false; std::string title; };
bool isAltTabEligible(const RawWindow& w);
}
