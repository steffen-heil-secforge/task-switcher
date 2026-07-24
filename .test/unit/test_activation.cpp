#include "doctest/doctest.h"
#include "activation.hpp"
using namespace tsw;
TEST_CASE("entry on this side activates locally") { CHECK(shouldForegroundLocally("local","local")); }
TEST_CASE("entry on other side does not activate locally") { CHECK_FALSE(shouldForegroundLocally("server","local")); }
TEST_CASE("server activating its own window is local to it") { CHECK(shouldForegroundLocally("server","server")); }
TEST_CASE("parseHwnd parses hex") { CHECK(parseHwnd("0x2A") == (void*)0x2A); }
