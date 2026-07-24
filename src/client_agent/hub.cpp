#include "hub.hpp"
#include "activation.hpp"
#include <windows.h>
#include <fstream>
#include <algorithm>
#include <mutex>
namespace tsw {

void logLine(const std::string& s){
    static const char* dir = std::getenv("TSW_LOG");   // opt-in; no diagnostic files in a normal install
    if(!dir || !*dir) return;
    static std::mutex m;
    std::lock_guard<std::mutex> g(m);
    std::ofstream f(std::string(dir) + "\\client.log", std::ios::app);
    f << GetTickCount64() << "  " << s << "\n";
}

std::string Hub::uniquify(const std::string& hostname) const {
    std::string base = hostname.empty() ? "server" : hostname;
    std::string cand = base;
    int n = 2;
    auto taken = [&](const std::string& d){
        for (auto& kv : conns_) if (kv.second.display == d) return true;
        return false;
    };
    while (taken(cand)) cand = base + "#" + std::to_string(n++);
    return cand;
}

std::string Hub::displayFor(const std::string& connKey) const {
    if (connKey == "local") return localName_;
    auto it = conns_.find(connKey);
    if (it != conns_.end() && !it->second.display.empty()) return it->second.display;
    return connKey;   // before Hello arrives
}

void Hub::setLocalWindows(const std::vector<WindowInfo>& ws){
    std::lock_guard<std::mutex> g(mu_); model_.applySnapshot("local", ws);
}
void Hub::setLocalName(const std::string& name){
    std::lock_guard<std::mutex> g(mu_); if (!name.empty()) localName_ = name;
}
std::string Hub::localName() const { std::lock_guard<std::mutex> g(mu_); return localName_; }
void Hub::setSendFor(const std::string& connKey, std::function<void(const std::string&)> send){
    std::lock_guard<std::mutex> g(mu_); conns_[connKey].send = std::move(send);
}
void Hub::setPidResolver(std::function<void*(int)> r){
    std::lock_guard<std::mutex> g(mu_); resolvePid_ = std::move(r);
}

void Hub::onBytes(const std::string& connKey, const char* data, int n){
    std::lock_guard<std::mutex> g(mu_);
    Conn& c = conns_[connKey];
    c.reader.feed(data, n);
    Message m;
    while (c.reader.next(m)) {
        switch (m.type) {
            case MsgType::Hello:    if (c.display.empty()) c.display = uniquify(m.hostname); logLine("rx Hello on " + connKey + " host=" + m.hostname); break;
            case MsgType::Snapshot: model_.applySnapshot(connKey, m.windows); logLine("rx Snapshot on " + connKey + " windows=" + std::to_string(m.windows.size())); break;
            case MsgType::Delta:    model_.applyDelta(connKey, m.added, m.removed, m.updated); break;
            case MsgType::Bridge:   c.mstscPid = m.requestId; if (resolvePid_) c.mstsc = resolvePid_(m.requestId); break;  // requestId carries PID
            case MsgType::Hotkey:   logLine("rx Hotkey on " + connKey + " fg=" + m.targetHwnd);
                                    // The message carries the focused window's info: upsert it so the picker
                                    // can show the preselected row immediately (remove-then-add avoids a
                                    // duplicate; the next Snapshot replaces the whole list anyway).
                                    if (!m.windows.empty()) model_.applyDelta(connKey, m.windows, {m.windows[0].hwnd}, {});
                                    if (onHotkey_) onHotkey_(displayFor(connKey), m.targetHwnd); break;  // PostMessage only — called under mu_
            default: break;
        }
    }
}
void Hub::onDisconnect(const std::string& connKey){
    logLine("onDisconnect key=" + connKey);
    std::lock_guard<std::mutex> g(mu_);
    model_.dropEndpoint(connKey);
    conns_.erase(connKey);
}
std::vector<Entry> Hub::snapshot() const {
    std::lock_guard<std::mutex> g(mu_);
    // mstsc PIDs of connections that HAVE our agent (Bridge received). Their remote windows are
    // listed under the server's own endpoint, so the local mstsc window for them is redundant.
    // Connections without our agent never send Bridge, so their mstsc window stays listed.
    std::vector<int> bridged;
    for (auto& kv : conns_) if (kv.second.mstscPid > 0) bridged.push_back(kv.second.mstscPid);
    std::vector<Entry> out;
    for (auto& e : model_.all()) {
        if (e.endpointId == "local" && e.w.pid != 0 &&
            std::find(bridged.begin(), bridged.end(), e.w.pid) != bridged.end()) continue;
        Entry m = e; m.endpointId = displayFor(e.endpointId);   // connKey -> display name
        out.push_back(m);
    }
    // Per-system LRU: entries stay grouped by endpoint, and within each endpoint they keep the
    // z-order the OS returned (foreground-first), refreshed every EnumRequest. No global reorder.
    return out;
}
void Hub::setLocalEnumerator(std::function<std::vector<WindowInfo>()> f){
    std::lock_guard<std::mutex> g(mu_); localEnum_ = std::move(f);
}
void Hub::setOnHotkey(std::function<void(const std::string&, const std::string&)> f){
    std::lock_guard<std::mutex> g(mu_); onHotkey_ = std::move(f);
}
void Hub::refreshLocal(){
    std::function<std::vector<WindowInfo>()> f;
    { std::lock_guard<std::mutex> g(mu_); f = localEnum_; }
    if (!f) return;
    auto ws = f();                                   // enumerate outside the lock
    std::lock_guard<std::mutex> g(mu_); model_.applySnapshot("local", ws);
}
void Hub::requestEnum(){
    std::vector<std::function<void(const std::string&)>> sends;
    size_t nconns;
    { std::lock_guard<std::mutex> g(mu_); nconns = conns_.size(); for (auto& kv : conns_) if (kv.second.send) sends.push_back(kv.second.send); }
    logLine("requestEnum conns=" + std::to_string(nconns) + " sends=" + std::to_string(sends.size()));
    Message m; m.type = MsgType::EnumRequest;
    std::string f = encodeFrame(m);
    for (auto& s : sends) s(f);
}
void Hub::activate(const Entry& e){
    std::function<void(const std::string&)> send; std::function<void*(int)> resolve;
    void* bridge = nullptr; int pid = 0; bool isLocal = true;
    {
        std::lock_guard<std::mutex> g(mu_);
        resolve = resolvePid_;
        // Local unless the endpoint name matches a live server connection (local rows now display
        // as this PC's hostname, so we can't test against the literal "local").
        for (auto& kv : conns_) if (kv.second.display == e.endpointId) { isLocal = false; send = kv.second.send; bridge = kv.second.mstsc; pid = kv.second.mstscPid; break; }
    }
    if (isLocal) {
        logLine("activate local hwnd=" + e.w.hwnd);
        if (e.w.hwnd == kDesktopHwnd) showDesktop(); else foregroundWindow(parseHwnd(e.w.hwnd));
        return;
    }
    // Re-resolve the bridge window NOW: mstsc destroys/recreates its top-level window (connect
    // dialog -> session window, windowed <-> fullscreen), so an HWND cached at Bridge time can
    // be stale, which made cross-session switching silently do nothing on the client side.
    if (pid && resolve) { if (void* fresh = resolve(pid)) bridge = fresh; }
    char bt[256] = ""; if (bridge) GetWindowTextA((HWND)bridge, bt, sizeof bt);
    logLine("activate ep=" + e.endpointId + " sendSet=" + std::to_string((bool)send)
            + " pid=" + std::to_string(pid) + " bridgeValid=" + std::to_string(bridge && IsWindow((HWND)bridge))
            + " bridgeTitle=[" + bt + "]");
    // These are INDEPENDENT: revealing THIS mstsc window (local) must not wait on delivering the
    // ActivateRequest (into the connection). Foreground mstsc FIRST/unconditionally, then send —
    // so a slow/stalled send never keeps the window from coming forward.
    if (bridge) foregroundWindow(bridge);
    if (send) { Message m; m.type=MsgType::ActivateRequest; m.endpointId=e.endpointId; m.targetHwnd=e.w.hwnd; send(encodeFrame(m)); }
}
}
