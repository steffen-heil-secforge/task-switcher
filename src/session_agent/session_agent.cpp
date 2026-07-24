#include <windows.h>
#include <wtsapi32.h>
#include <string>
#include <mutex>
#include "protocol.hpp"
#include "version.hpp"
#include "win_enumerator.hpp"
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "user32.lib")
#include <fstream>
#include <thread>
#include <atomic>
#include <cstdio>
#include "activation.hpp"
using namespace tsw;

static void L(const std::string& s){
    static const char* dir = std::getenv("TSW_LOG");   // opt-in diagnostics only
    if(!dir || !*dir) return;
    std::ofstream f(std::string(dir) + "\\session.log", std::ios::app);
    f << GetTickCount64() << "  " << s << "\n";
}

// ---- Channel state -------------------------------------------------------------------------
// The DVC channel is written from two threads (the main loop's snapshots, the hotkey thread) and
// its lifetime (open/close/reopen) is driven by the main loop. g_chMu serializes writes + the
// open/close; the blocking read runs OUTSIDE the lock (on a captured handle) so it never stalls a
// write. g_reopen requests a reconnect from whoever detects staleness (session-change notify or a
// failed send); the main loop acts on it.
static HANDLE            g_channel = nullptr;
static std::mutex        g_chMu;
static std::atomic<bool> g_reopen{false};
static Message           g_hello;             // re-sent on connect / reconnect / every EnumRequest

static bool sendMsgLocked(const Message& m){   // caller holds g_chMu
    if(!g_channel) return false;
    std::string f = encodeFrame(m);
    ULONG w = 0; BOOL ok = WTSVirtualChannelWrite(g_channel, (PCHAR)f.data(), (ULONG)f.size(), &w);
    return ok != 0 && w == f.size();
}
static bool sendMsg(const Message& m){
    std::lock_guard<std::mutex> g(g_chMu);
    bool ok = sendMsgLocked(m);
    if(!ok) g_reopen = true;   // a failed send means the channel is stale -> reconnect
    return ok;
}
static bool sendSnapshot(){
    Message m; m.type = MsgType::Snapshot; m.endpointId = "server"; m.windows = enumerateWindows();
    return sendMsg(m);
}
// Wait until this session is an RDP session, then open the DVC channel (retry until success).
static HANDLE openChannelRaw(){
    for (;;) {
        if (GetSystemMetrics(SM_REMOTESESSION)) {
            HANDLE ch = WTSVirtualChannelOpenEx(WTS_CURRENT_SESSION, (LPSTR)"TSWLIST", WTS_CHANNEL_OPTION_DYNAMIC);
            L("openChannel ch=" + std::to_string((uintptr_t)ch) + " err=" + std::to_string(GetLastError()));
            if (ch) return ch;
        }
        Sleep(1500);
    }
}
// Close the current channel (if any) and open a fresh one, then re-announce (Hello + a snapshot).
static void reopenChannel(){
    {
        std::lock_guard<std::mutex> g(g_chMu);
        if (g_channel) { WTSVirtualChannelClose(g_channel); g_channel = nullptr; }
        g_channel = openChannelRaw();
    }
    L("channel reopened");
    sendMsg(g_hello);
    sendSnapshot();
}

// ---- Hotkey (Ctrl+^) inside the RDP session ------------------------------------------------
// mstsc forwards keystrokes to this server while it has focus, so the client-side hook never sees
// Ctrl+^ there. Detect it here and tell the client to pop the picker. The detection runs on the
// LL-hook thread (which must return fast), so it only sets an event; a dedicated sender thread does
// the (potentially blocking) channel write, keeping the hook snappy.
static HANDLE            g_hotkeyEvt = nullptr;     // auto-reset: signalled on Ctrl+^
static std::atomic<void*> g_hotkeyFg{nullptr};      // window focused when the hotkey was pressed
static const DWORD kHotkeyScan = 0x29;              // top-left "^" (scancode), matches the client
// When we swallow Ctrl+^, the picker pops on the CLIENT and steals foreground, so the user's Ctrl
// key-up may land on the client rather than here. Release Ctrl on this server so it doesn't linger
// as a held modifier for the next keystrokes. (Harmless if it wasn't held.)
static void releaseCtrl(){
    INPUT in[2]={};
    in[0].type=INPUT_KEYBOARD; in[0].ki.wVk=VK_LCONTROL; in[0].ki.dwFlags=KEYEVENTF_KEYUP;
    in[1].type=INPUT_KEYBOARD; in[1].ki.wVk=VK_RCONTROL; in[1].ki.dwFlags=KEYEVENTF_KEYUP;
    SendInput(2,in,sizeof(INPUT));
}
static LRESULT CALLBACK kbProc(int code, WPARAM wp, LPARAM lp){
    if (code == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        if (k->flags & LLKHF_INJECTED) return CallNextHookEx(nullptr, code, wp, lp);
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && k->scanCode == kHotkeyScan) {
            g_hotkeyFg = (void*)GetForegroundWindow();
            releaseCtrl();           // don't leave Ctrl stuck on this server after the picker takes over
            SetEvent(g_hotkeyEvt);   // hand off to the sender thread; keep the hook fast
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
static void hookThread(){
    HHOOK h = SetWindowsHookExW(WH_KEYBOARD_LL, kbProc, GetModuleHandleW(nullptr), 0);
    L("hookThread: hook=" + std::to_string(h != nullptr));
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}
// Build + send the Hotkey message when the hook signals. targetHwnd carries the focused window so
// the picker can preselect it, and windows[0] carries its full info so the row can be shown at once.
static void hotkeyThread(){
    for (;;) {
        WaitForSingleObject(g_hotkeyEvt, INFINITE);
        HWND fg = (HWND)g_hotkeyFg.load();
        wchar_t cls[64] = L""; if (fg) GetClassNameW(fg, cls, 64);
        bool onDesktop = !lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW");
        char fb[32]; sprintf_s(fb, "0x%llX", (unsigned long long)(uintptr_t)fg);
        Message h; h.type = MsgType::Hotkey;
        if (onDesktop) {
            h.targetHwnd = "desktop";
            WindowInfo w; w.hwnd = "desktop"; w.title = "Desktop"; h.windows.push_back(w);
            L("hotkey -> Hotkey to client, fg=desktop");
            sendMsg(h);
            continue;
        }
        h.targetHwnd = fb;
        L(std::string("hotkey -> Hotkey to client, fg=") + fb);
        if (IsWindow(fg)) {
            WindowInfo w; w.hwnd = fb;
            wchar_t tw[512] = L""; GetWindowTextW(fg, tw, 512);
            char t[1024] = ""; WideCharToMultiByte(CP_UTF8, 0, tw, -1, t, sizeof t, nullptr, nullptr);
            w.title = t;
            DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            char buf[MAX_PATH] = ""; if (p) { DWORD n = MAX_PATH; QueryFullProcessImageNameA(p, 0, buf, &n); CloseHandle(p); }
            std::string s = buf; auto pos = s.find_last_of("\\/");
            w.process = pos == std::string::npos ? s : s.substr(pos + 1);
            h.windows.push_back(w);
        }
        sendMsg(h);
    }
}

// ---- Session-change notifications (reconnect detection, replaces the heartbeat) ------------
// A message-only window registered for this session's notifications. On an RDP reconnect the
// server-side channel handle goes stale; WTS_REMOTE_CONNECT (and logon/unlock) tells us to reopen.
static LRESULT CALLBACK notifyProc(HWND h, UINT m, WPARAM wp, LPARAM lp){
    if (m == WM_WTSSESSION_CHANGE) {
        if (wp == WTS_REMOTE_CONNECT || wp == WTS_SESSION_LOGON || wp == WTS_SESSION_UNLOCK) {
            g_reopen = true; L("session change wp=" + std::to_string((int)wp) + " -> reopen");
        }
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}
static void notifyThread(){
    WNDCLASSW wc{}; wc.lpfnWndProc = notifyProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"TSWNotify";
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(0, L"TSWNotify", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (h) WTSRegisterSessionNotification(h, NOTIFY_FOR_THIS_SESSION);
    L("notifyThread: window=" + std::to_string(h != nullptr));
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}

int runSessionAgent(){   // entry when this machine IS an RDP session (a server the user is connected to)
    L("session agent started");
    g_hotkeyEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // auto-reset
    std::thread(hookThread).detach();     // hotkey detection inside the RDP session
    std::thread(hotkeyThread).detach();   // event-driven hotkey sender (no polling)
    std::thread(notifyThread).detach();   // reconnect detection (replaces the heartbeat)

    { std::lock_guard<std::mutex> g(g_chMu); g_channel = openChannelRaw(); }
    L("channel opened");

    g_hello.type = MsgType::Hello; g_hello.endpointId = "server"; g_hello.protocolVersion = kProtocolVersion;
    char host[256] = ""; DWORD hn = 256; GetComputerNameA(host, &hn); g_hello.hostname = host;
    DWORD sid = 0; ProcessIdToSessionId(GetCurrentProcessId(), &sid); g_hello.sessionId = (int)sid;
    sendMsg(g_hello);
    sendSnapshot();   // one initial snapshot on connect; thereafter ON DEMAND (EnumRequest)

    // Fully event-driven: the loop parks in a blocking channel read and does nothing while idle —
    // no heartbeat, no timers. It wakes the instant the client sends (EnumRequest/ActivateRequest);
    // the 5 s timeout is only a safety re-check of g_reopen (set by the session-change notify or a
    // failed send). CHANNEL_PDU_HEADER {ULONG length; ULONG flags;} FIRST=1 LAST=2.
    const ULONG CH_FIRST = 0x1, CH_LAST = 0x2;
    FrameReader ir; char ibuf[8192]; std::string chunkBuf;
    for (;;) {
        if (g_reopen.exchange(false)) { reopenChannel(); ir = FrameReader{}; chunkBuf.clear(); continue; }
        HANDLE ch; { std::lock_guard<std::mutex> g(g_chMu); ch = g_channel; }
        ULONG rd = 0;
        if (WTSVirtualChannelRead(ch, 5000, ibuf, sizeof ibuf, &rd) && rd >= 8) {
            ULONG flags = *reinterpret_cast<ULONG*>(ibuf + 4);
            if (flags & CH_FIRST) chunkBuf.clear();
            chunkBuf.append(ibuf + 8, rd - 8);
            if (flags & CH_LAST) {
                ir.feed(chunkBuf.data(), chunkBuf.size());
                chunkBuf.clear();
                Message m;
                while (ir.next(m)) {
                    if (m.type == MsgType::EnumRequest) {
                        // Re-send Hello too: if the client agent restarted, only its pipe reconnected
                        // (this DVC channel stayed up), so the new connection never got the Hello sent
                        // at channel-open and wouldn't know this machine's hostname -> the client would
                        // mis-route activations as "local". EnumRequest (every picker open) heals it.
                        L("EnumRequest -> hello+snapshot"); sendMsg(g_hello); sendSnapshot();
                    }
                    else if (m.type == MsgType::ActivateRequest) {
                        L("ActivateRequest hwnd=" + m.targetHwnd);
                        if (m.targetHwnd == kDesktopHwnd) showDesktop();
                        else foregroundWindow(parseHwnd(m.targetHwnd));
                        Sleep(200);
                        HWND fg = GetForegroundWindow(); char t[256] = ""; GetWindowTextA(fg, t, sizeof t);
                        L(std::string("ActivateRequest done, foreground='") + t + "'");
                    }
                }
            }
        }
    }
}
