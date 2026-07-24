#include <windows.h>
#include <thread>
#include <chrono>
#include <fstream>
#include <cstdlib>
#include <string>
#include <vector>
#include <utility>
#include "hub.hpp"
#include "pipe.hpp"
#include "win_enumerator.hpp"
using namespace tsw;

Hub g_hub;
namespace tsw { void showPicker(Hub&); }   // picker.cpp
static void CL(const std::string& s);      // forward decl (defined below)

// Test hook: when TSW_ACTIVATE_FILE is set, watch it; when its (title substring) contents
// change, activate the first merged-model entry whose title contains that substring.
static void activateWatchLoop(std::string path){
    CL("activate-watch started path=" + path);
    std::string done, announced;
    for (;;) {
        std::string want; { std::ifstream f(path); std::getline(f, want); }
        while (!want.empty() && (want.back()=='\r' || want.back()=='\n' || want.back()==' ')) want.pop_back();
        if (want.empty()) { done.clear(); announced.clear(); }   // empty file re-arms the same target
        if (!want.empty() && want != done) {          // keep RETRYING until we actually fire
            if (want != announced) { announced = want; CL("activate-watch target=[" + want + "]"); }
            for (auto& e : g_hub.snapshot()) if (e.w.title.find(want) != std::string::npos) {
                CL("activate-hook firing for [" + e.endpointId + "] " + e.w.title);
                g_hub.activate(e); done = want; break;    // mark done only after activating
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// Resolve a PID (the plugin's host mstsc process) to its top-level window — that connection's
// bridge window. Used by the hub when a Bridge message arrives.
struct PidFind { DWORD pid; HWND hwnd; };
static void* findWindowForPid(int pid){
    PidFind pf{ (DWORD)pid, nullptr };
    EnumWindows([](HWND h, LPARAM lp)->BOOL{
        auto* f = (PidFind*)lp;
        if (!IsWindowVisible(h) || GetAncestor(h, GA_ROOTOWNER) != h) return TRUE;
        DWORD wpid=0; GetWindowThreadProcessId(h,&wpid);
        if (wpid == f->pid) { f->hwnd = h; return FALSE; }
        return TRUE;
    }, (LPARAM)&pf);
    return pf.hwnd;
}

static void CL(const std::string& s){ tsw::logLine(s); }   // thread-safe (shared mutex in hub.cpp)

// Test hook: when TSW_DUMP_FILE is set, periodically refresh local windows and
// write the merged model to that file so an automated E2E can assert on it.
static void dumpLoop(std::string path){
    for (;;) {
        g_hub.requestEnum();   // test-mode: keep the model fresh (real UI enums on hotkey)
        g_hub.setLocalWindows(enumerateWindows());
        std::ofstream f(path, std::ios::trunc);
        for (auto& e : g_hub.snapshot()) f << "[" << e.endpointId << "] " << e.w.title << " (" << e.w.process << ")\n";
        f.flush();
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

// Launch mstsc in THIS (interactive) session — the agent already runs in session 1, so a
// plain CreateProcess lands in-session exactly like a user double-clicking the .rdp.
static void launchMstsc(const std::string& rdp){
    std::string cmd = "mstsc.exe \"" + rdp + "\"";
    STARTUPINFOA si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
    std::vector<char> c(cmd.begin(), cmd.end()); c.push_back(0);
    if (CreateProcessA(nullptr, c.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
}
static bool endpointConnected(const std::string& host){
    for (auto& e : g_hub.snapshot()) if (e.endpointId == host) return true;
    return false;
}
// Connection watchdog: TSW_CONNECT = "rdpPath|hostname;rdpPath|hostname". Launch mstsc only
// for servers whose endpoint isn't connected yet; re-check and relaunch missing ones (no kill,
// so it never disturbs a session that did connect). Gives reliable multi-session bring-up.
static void connectManager(std::string list){
    std::vector<std::pair<std::string,std::string>> targets;
    size_t p = 0;
    while (p <= list.size()) {
        auto semi = list.find(';', p);
        std::string item = list.substr(p, semi==std::string::npos? std::string::npos : semi-p);
        auto bar = item.find('|');
        if (bar != std::string::npos) targets.push_back({ item.substr(0,bar), item.substr(bar+1) });
        if (semi == std::string::npos) break; p = semi + 1;
    }
    for (int attempt = 0; attempt < 30; attempt++) {
        bool allUp = true;
        for (auto& t : targets) if (!endpointConnected(t.second)) { allUp = false; CL("connectManager: launching mstsc for " + t.second); launchMstsc(t.first); }
        if (allUp) { CL("connectManager: all servers connected"); return; }
        std::this_thread::sleep_for(std::chrono::seconds(10));   // let launches establish before re-checking
    }
    CL("connectManager: gave up");
}

// Serve one pipe connection (one mstsc) for its lifetime, re-accepting after disconnect.
// Each connection is a distinct hub endpoint keyed by `key`.
static void serveConn(std::string key){
    for (;;) {
        PipeServer s;
        if (!s.start(kPipeName)) return;
        if (!s.waitForClient()) { s.close(); continue; }
        CL(key + " connected");
        g_hub.setSendFor(key, [&s, key](const std::string& f){ bool ok = s.write(f.data(), (int)f.size()); CL(key + " send bytes=" + std::to_string(f.size()) + " ok=" + std::to_string(ok?1:0)); });
        // Event-driven: block in an overlapped ReadFile until data arrives (no polling). The pipe
        // is overlapped, so this read does NOT serialize with the hub's write from the picker
        // thread (separate completion events). Read returns <=0 on disconnect -> re-accept.
        char buf[8192];
        for (;;) {
            int n = s.read(buf, sizeof(buf));
            if (n <= 0) { CL(key + " read<=0 n=" + std::to_string(n) + " err=" + std::to_string(GetLastError())); break; }
            g_hub.onBytes(key, buf, n);
        }
        g_hub.setSendFor(key, nullptr);   // avoid dangling capture of s
        g_hub.onDisconnect(key);
        s.close();
    }
}

int runClientAgent(){   // entry when this machine is NOT an RDP session (the user's own PC)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);   // crisp panel at any DPI
    CL("client agent started pid=" + std::to_string(GetCurrentProcessId()));
    { char host[256] = ""; DWORD hn = 256; if (GetComputerNameA(host, &hn)) g_hub.setLocalName(host); }
    g_hub.setLocalWindows(enumerateWindows());
    g_hub.setLocalEnumerator([]{ return enumerateWindows(); });   // re-enumerated on each hotkey
    g_hub.setPidResolver([](int pid){ return findWindowForPid(pid); });
    if (const char* dp = std::getenv("TSW_DUMP_FILE")) { std::string p = dp; std::thread([p]{ dumpLoop(p); }).detach(); }
    if (const char* ap = std::getenv("TSW_ACTIVATE_FILE")) { std::string p = ap; std::thread([p]{ activateWatchLoop(p); }).detach(); }
    if (const char* rc = std::getenv("TSW_CONNECT")) { std::string l = rc; std::thread([l]{ connectManager(l); }).detach(); }
    // Accept several concurrent mstsc connections, each a distinct endpoint.
    for (int i = 0; i < 4; i++) { std::string key = "conn" + std::to_string(i); std::thread([key]{ serveConn(key); }).detach(); }
    showPicker(g_hub);   // blocks on the UI message loop
    return 0;
}
