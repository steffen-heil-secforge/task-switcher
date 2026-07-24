#include "doctest/doctest.h"
#include "protocol.hpp"
using namespace tsw;

TEST_CASE("snapshot round-trips through frame codec") {
    Message m;
    m.type = MsgType::Snapshot;
    m.endpointId = "local";
    m.windows = { {"0x1234", "Notepad", "notepad.exe"} };

    std::string frame = encodeFrame(m);
    CHECK(frame.size() > 4);

    FrameReader r;
    r.feed(frame.data(), frame.size());
    Message out;
    REQUIRE(r.next(out));
    CHECK(out.type == MsgType::Snapshot);
    CHECK(out.endpointId == "local");
    REQUIRE(out.windows.size() == 1);
    CHECK(out.windows[0].title == "Notepad");
    CHECK_FALSE(r.next(out)); // no second message
}

TEST_CASE("activate request round-trips") {
    Message m; m.type = MsgType::ActivateRequest; m.endpointId = "server";
    m.targetHwnd = "0x2A2A"; m.requestId = 7;
    std::string frame = encodeFrame(m);
    FrameReader r; r.feed(frame.data(), frame.size());
    Message out; REQUIRE(r.next(out));
    CHECK(out.type == MsgType::ActivateRequest);
    CHECK(out.targetHwnd == "0x2A2A");
    CHECK(out.requestId == 7);
}

TEST_CASE("FrameReader handles split and concatenated frames") {
    Message a; a.type = MsgType::Ping;
    Message b; b.type = MsgType::Pong;
    std::string buf = encodeFrame(a) + encodeFrame(b);

    FrameReader r; Message out;
    r.feed(buf.data(), 3);              // partial first frame
    CHECK_FALSE(r.next(out));
    r.feed(buf.data() + 3, buf.size() - 3);
    REQUIRE(r.next(out)); CHECK(out.type == MsgType::Ping);
    REQUIRE(r.next(out)); CHECK(out.type == MsgType::Pong);
}
