#include "doctest/doctest.h"
#include "hub.hpp"
#include "protocol.hpp"
using namespace tsw;

static void feed(Hub& h, const std::string& key, const std::string& bytes){ h.onBytes(key, bytes.data(), (int)bytes.size()); }
static Message hello(const std::string& host){ Message m; m.type=MsgType::Hello; m.hostname=host; return m; }
static Message snap(std::initializer_list<WindowInfo> ws){ Message m; m.type=MsgType::Snapshot; m.windows=ws; return m; }

TEST_CASE("connection windows are tagged with the Hello hostname (snapshot-before-hello)") {
    Hub h;
    h.setLocalWindows({ {"0x1","Local","l.exe"} });
    // session agent sends Snapshot first, then Hello — hub must still tag by hostname
    feed(h, "cA", encodeFrame(snap({ {"0x9","Remote","r.exe"} })) + encodeFrame(hello("HOST-A")));
    auto all = h.snapshot();
    CHECK(all.size() == 2);
    bool sawLocal=false, sawA=false;
    for (auto& e : all) { if (e.endpointId=="local") sawLocal=true; if (e.endpointId=="HOST-A") sawA=true; }
    CHECK(sawLocal); CHECK(sawA);
}

TEST_CASE("two connections aggregate as distinct endpoints; duplicate hostnames uniquified") {
    Hub h;
    feed(h, "cA", encodeFrame(hello("HOST")) + encodeFrame(snap({ {"0xA","A","a.exe"} })));
    feed(h, "cB", encodeFrame(hello("HOST")) + encodeFrame(snap({ {"0xB","B","b.exe"} })));
    auto all = h.snapshot();
    REQUIRE(all.size() == 2);
    std::string e0=all[0].endpointId, e1=all[1].endpointId;
    CHECK(e0 != e1);                       // uniquified: "HOST" and "HOST#2"
    CHECK((e0=="HOST" || e1=="HOST"));
}

TEST_CASE("activation routes to the correct connection's send") {
    Hub h;
    feed(h, "cA", encodeFrame(hello("HOST-A")) + encodeFrame(snap({ {"0xA","A","a.exe"} })));
    feed(h, "cB", encodeFrame(hello("HOST-B")) + encodeFrame(snap({ {"0xB","B","b.exe"} })));
    bool firedA=false, firedB=false;
    h.setSendFor("cA", [&](const std::string&){ firedA=true; });
    h.setSendFor("cB", [&](const std::string&){ firedB=true; });
    for (auto& e : h.snapshot()) if (e.endpointId=="HOST-B") h.activate(e);
    CHECK(firedB);
    CHECK_FALSE(firedA);
}

TEST_CASE("disconnect drops that connection's windows only") {
    Hub h;
    feed(h, "cA", encodeFrame(hello("HOST-A")) + encodeFrame(snap({ {"0xA","A","a.exe"} })));
    feed(h, "cB", encodeFrame(hello("HOST-B")) + encodeFrame(snap({ {"0xB","B","b.exe"} })));
    REQUIRE(h.snapshot().size() == 2);
    h.onDisconnect("cA");
    auto all = h.snapshot();
    REQUIRE(all.size() == 1);
    CHECK(all[0].endpointId == "HOST-B");
}
