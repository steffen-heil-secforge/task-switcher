#include "doctest/doctest.h"
#include "merged_model.hpp"
using namespace tsw;

TEST_CASE("snapshots from two endpoints add up") {
    MergedModel m;
    m.applySnapshot("local",     { {"0x1","A","a.exe"} });
    m.applySnapshot("session-1", { {"0x9","B","b.exe"}, {"0xA","C","c.exe"} });
    CHECK(m.all().size() == 3);
}
TEST_CASE("delta add/remove/update mutate one endpoint only") {
    MergedModel m;
    m.applySnapshot("local", { {"0x1","A","a.exe"} });
    m.applyDelta("local", { {"0x2","D","d.exe"} }, {}, {});
    CHECK(m.all().size() == 2);
    m.applyDelta("local", {}, { "0x1" }, {});
    CHECK(m.all().size() == 1);
    m.applyDelta("local", {}, {}, { {"0x2","D-renamed","d.exe"} });
    REQUIRE(m.all().size() == 1);
    CHECK(m.all()[0].w.title == "D-renamed");
}
TEST_CASE("dropEndpoint removes its windows (disconnect)") {
    MergedModel m;
    m.applySnapshot("local",     { {"0x1","A","a.exe"} });
    m.applySnapshot("session-1", { {"0x9","B","b.exe"} });
    m.dropEndpoint("session-1");
    REQUIRE(m.all().size() == 1);
    CHECK(m.all()[0].endpointId == "local");
}
