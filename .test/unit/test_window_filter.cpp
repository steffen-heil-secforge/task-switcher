#include "doctest/doctest.h"
#include "window_filter.hpp"
using namespace tsw;
static RawWindow ok(){ return {true,true,false,false,"Real Window"}; }

TEST_CASE("a normal visible root-owner window with a title is eligible") { CHECK(isAltTabEligible(ok())); }
TEST_CASE("invisible window excluded")     { auto w=ok(); w.visible=false;     CHECK_FALSE(isAltTabEligible(w)); }
TEST_CASE("non-root-owner excluded")       { auto w=ok(); w.isRootOwner=false; CHECK_FALSE(isAltTabEligible(w)); }
TEST_CASE("tool window excluded")          { auto w=ok(); w.toolWindow=true;   CHECK_FALSE(isAltTabEligible(w)); }
TEST_CASE("cloaked (UWP phantom) excluded"){ auto w=ok(); w.cloaked=true;      CHECK_FALSE(isAltTabEligible(w)); }
TEST_CASE("empty title excluded")          { auto w=ok(); w.title="";          CHECK_FALSE(isAltTabEligible(w)); }
