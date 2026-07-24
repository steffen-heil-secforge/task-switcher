#pragma once
#include <string>
#include <vector>
namespace tsw {
struct WindowInfo {
    std::string hwnd, title, process;
    int pid = 0;              // local only, not serialized
    std::string iconPng;      // base64-encoded 32x32 PNG of the window/app icon ("" if none)
};
enum class MsgType { Hello, Snapshot, Delta, Ping, Pong, ActivateRequest, ActivateResult, Bridge, EnumRequest, Hotkey };
struct Message {
    MsgType type{};
    std::string endpointId;
    std::vector<WindowInfo> windows, added, updated;
    std::vector<std::string> removed;
    std::string hostname;
    int sessionId = 0;
    int protocolVersion = 0;
    std::string targetHwnd;   // ActivateRequest: window to bring to front
    int requestId = 0;
    int status = 0;           // ActivateResult
};
std::string encodeFrame(const Message& m);
struct FrameReader {
    void feed(const char* data, size_t n);
    bool next(Message& out);
private:
    std::string buf_;
};
}
