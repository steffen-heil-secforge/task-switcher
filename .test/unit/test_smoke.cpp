#include "doctest/doctest.h"
#include "version.hpp"
TEST_CASE("protocol version is 1") { CHECK(tsw::kProtocolVersion == 1); }
