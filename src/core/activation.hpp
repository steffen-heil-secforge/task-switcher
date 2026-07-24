#pragma once
#include <string>
namespace tsw {
// Pure decision: does an entry belong to the side running this picker?
bool shouldForegroundLocally(const std::string& entryEndpointId, const std::string& selfEndpointId);
// Parse "0x1234" into an opaque HWND value.
void* parseHwnd(const std::string& hex);
// Win32 (activation_win.cpp): restore if minimized + SetForegroundWindow with the
// foreground-lock workaround. No-op on a non-Windows build (not compiled there).
void foregroundWindow(void* hwnd);
// Minimize all windows on this machine and focus the desktop (the synthetic "Desktop" entry).
void showDesktop();
// Sentinel hwnd string for the synthetic per-machine "Desktop" entry.
inline const char* kDesktopHwnd = "desktop";
}
