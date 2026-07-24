#include "protocol.hpp"
#include "json.hpp"
#include <cstdint>
namespace tsw {
static const char* typeName(MsgType t) {
    switch (t) { case MsgType::Hello: return "hello"; case MsgType::Snapshot: return "snapshot";
        case MsgType::Delta: return "delta"; case MsgType::Ping: return "ping";
        case MsgType::ActivateRequest: return "activate_req"; case MsgType::ActivateResult: return "activate_res";
        case MsgType::Bridge: return "bridge";
        case MsgType::EnumRequest: return "enum_req";
        case MsgType::Hotkey: return "hotkey";
        default: return "pong"; }
}
static MsgType typeFrom(const std::string& s) {
    if (s=="hello") return MsgType::Hello; if (s=="snapshot") return MsgType::Snapshot;
    if (s=="delta") return MsgType::Delta; if (s=="ping") return MsgType::Ping;
    if (s=="activate_req") return MsgType::ActivateRequest; if (s=="activate_res") return MsgType::ActivateResult;
    if (s=="bridge") return MsgType::Bridge;
    if (s=="enum_req") return MsgType::EnumRequest;
    if (s=="hotkey") return MsgType::Hotkey;
    return MsgType::Pong;
}
static json winToJson(const WindowInfo& w){ return json{{"hwnd",w.hwnd},{"title",w.title},{"process",w.process},{"icon",w.iconPng}}; }
static WindowInfo winFrom(const json& j){ WindowInfo w; w.hwnd=j.value("hwnd",""); w.title=j.value("title",""); w.process=j.value("process",""); w.iconPng=j.value("icon",""); return w; }

std::string encodeFrame(const Message& m) {
    json j;
    j["type"] = typeName(m.type);
    j["endpointId"] = m.endpointId;
    j["hostname"] = m.hostname;
    j["sessionId"] = m.sessionId;
    j["protocolVersion"] = m.protocolVersion;
    j["targetHwnd"] = m.targetHwnd;
    j["requestId"] = m.requestId;
    j["status"] = m.status;
    for (auto& w : m.windows) j["windows"].push_back(winToJson(w));
    for (auto& w : m.added)   j["added"].push_back(winToJson(w));
    for (auto& w : m.updated) j["updated"].push_back(winToJson(w));
    for (auto& r : m.removed) j["removed"].push_back(r);
    // error_handler_t::replace: never throw on malformed UTF-8 in a window title (substitutes
    // U+FFFD). A bad title byte previously threw here -> unhandled exception -> abort() dialog,
    // which crashed the agent and killed its hotkey hook.
    std::string body = j.dump(-1, ' ', false, json::error_handler_t::replace);
    uint32_t n = (uint32_t)body.size();
    std::string frame(4, '\0');
    frame[0]=char(n&0xFF); frame[1]=char((n>>8)&0xFF); frame[2]=char((n>>16)&0xFF); frame[3]=char((n>>24)&0xFF);
    frame += body;
    return frame;
}
void FrameReader::feed(const char* data, size_t n){ buf_.append(data, n); }
bool FrameReader::next(Message& out) {
    if (buf_.size() < 4) return false;
    uint32_t n = (uint8_t)buf_[0] | ((uint8_t)buf_[1]<<8) | ((uint8_t)buf_[2]<<16) | ((uint32_t)(uint8_t)buf_[3]<<24);
    if (buf_.size() < 4 + n) return false;
    json j = json::parse(buf_.substr(4, n), nullptr, false);
    buf_.erase(0, 4 + n);
    if (j.is_discarded()) return false;
    out = Message{};
    out.type = typeFrom(j.value("type", "pong"));
    out.endpointId = j.value("endpointId", "");
    out.hostname = j.value("hostname", "");
    out.sessionId = j.value("sessionId", 0);
    out.protocolVersion = j.value("protocolVersion", 0);
    out.targetHwnd = j.value("targetHwnd", "");
    out.requestId = j.value("requestId", 0);
    out.status = j.value("status", 0);
    if (j.contains("windows")) for (auto& w : j["windows"]) out.windows.push_back(winFrom(w));
    if (j.contains("added"))   for (auto& w : j["added"])   out.added.push_back(winFrom(w));
    if (j.contains("updated")) for (auto& w : j["updated"]) out.updated.push_back(winFrom(w));
    if (j.contains("removed")) for (auto& r : j["removed"]) out.removed.push_back(r.get<std::string>());
    return true;
}
}
