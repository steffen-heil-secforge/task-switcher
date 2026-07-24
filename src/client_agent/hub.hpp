#pragma once
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <functional>
#include "protocol.hpp"
#include "merged_model.hpp"
namespace tsw {
// Thread-safe append to <TSW_LOG>\client.log (opt-in: no-op unless the TSW_LOG env var is set;
// prepends a tick timestamp). Used from all
// client-agent threads; a single shared mutex avoids the silent line loss that per-call
// std::ofstream opens suffer under concurrent access on Windows (sharing violations).
void logLine(const std::string& s);
// Multi-session hub (client-only picker). Each mstsc connection is a distinct endpoint,
// keyed internally by a stable connection key; its display/routing name comes from the
// server's Hello hostname (uniquified). The model is keyed by connection key so it needs
// no re-tagging when Hello arrives after the first snapshot.
class Hub {
public:
    void setLocalWindows(const std::vector<WindowInfo>& ws);
    void setLocalName(const std::string& name);   // display label for local windows (this PC's hostname)
    std::string localName() const;
    void onBytes(const std::string& connKey, const char* data, int n);
    void onDisconnect(const std::string& connKey);
    std::vector<Entry> snapshot() const;   // Entry.endpointId = display name ("local" or hostname)

    void setSendFor(const std::string& connKey, std::function<void(const std::string&)> send);
    void setPidResolver(std::function<void*(int)> resolvePidToHwnd);
    void activate(const Entry& e);         // e.endpointId is a display name
    void requestEnum();                    // broadcast EnumRequest to every server (on picker pop-up)
    void setLocalEnumerator(std::function<std::vector<WindowInfo>()> f);
    void refreshLocal();                   // re-enumerate local windows now (on picker pop-up)
    // Server-side Ctrl+Alt+Space (focus was in RDP): (endpoint display name, hwnd focused there).
    void setOnHotkey(std::function<void(const std::string&, const std::string&)> f);
private:
    struct Conn {
        std::string display;                          // hostname (uniquified); empty until Hello
        FrameReader reader;
        std::function<void(const std::string&)> send; // write to this connection's pipe
        int mstscPid = 0;                             // this connection's mstsc process (from Bridge)
        void* mstsc = nullptr;                        // last resolved bridge window (refreshed on activate)
    };
    std::string displayFor(const std::string& connKey) const;   // "local" or conn.display or connKey
    std::string uniquify(const std::string& hostname) const;

    mutable std::mutex mu_;
    std::string localName_ = "local";         // display label for the "local" endpoint
    MergedModel model_;                       // keyed by connKey (and "local")
    std::map<std::string, Conn> conns_;       // connKey -> Conn
    std::function<void*(int)> resolvePid_;
    std::function<std::vector<WindowInfo>()> localEnum_;
    std::function<void(const std::string&, const std::string&)> onHotkey_;
};
}
