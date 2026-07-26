#include "doctest/doctest.h"
#include "protocol.hpp"
#include <cstdint>
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

static std::string rawFrame(const std::string& body) {
    uint32_t n = (uint32_t)body.size();
    std::string frame(4, '\0');
    frame[0]=char(n&0xFF); frame[1]=char((n>>8)&0xFF);
    frame[2]=char((n>>16)&0xFF); frame[3]=char((n>>24)&0xFF);
    return frame + body;
}

TEST_CASE("malformed frame schema is skipped without throwing or stranding the next frame") {
    Message good; good.type = MsgType::Hello; good.hostname = "HOST";
    std::string bytes = rawFrame(R"({"type":"hello","sessionId":"wrong type"})") + encodeFrame(good);
    FrameReader r; r.feed(bytes.data(), bytes.size());
    Message out;
    REQUIRE_NOTHROW(r.next(out));
    CHECK(out.type == MsgType::Hello);
    CHECK(out.hostname == "HOST");
    CHECK_FALSE(r.failed());
}

TEST_CASE("oversized frame poisons the reader without retaining subsequent input") {
    std::string header(4, '\xFF');
    FrameReader r; r.feed(header.data(), header.size());
    Message out;
    CHECK_FALSE(r.next(out));
    CHECK(r.failed());
    Message good; good.type = MsgType::Ping;
    std::string frame = encodeFrame(good);
    r.feed(frame.data(), frame.size());
    CHECK_FALSE(r.next(out));
}
