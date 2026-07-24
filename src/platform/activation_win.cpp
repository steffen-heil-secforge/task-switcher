#include "activation.hpp"
#include <windows.h>
#include <shldisp.h>   // IShellDispatch::MinimizeAll
namespace tsw {

void showDesktop(){
    // ToggleDesktop() = the real "Show Desktop" (exactly what Win+D does): the windows go to the
    // show-desktop state as a group, so Win+D (or invoking Desktop again) restores them. This is
    // unlike MinimizeAll(), which minimizes each window individually and can't be undone that way.
    bool init = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    IShellDispatch4* sd = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_Shell, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IShellDispatch4, (void**)&sd)) && sd) {
        sd->ToggleDesktop();
        sd->Release();
    }
    if (init) CoUninitialize();
}
void foregroundWindow(void* h){
    HWND hwnd = (HWND)h;
    if (!IsWindow(hwnd)) return;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    // Force foreground WITHOUT AttachThreadInput: an elevated agent attaching to mstsc's (lower-
    // integrity) input queue can wedge mstsc's message loop, leaving its window unclosable.
    // Zeroing the foreground-lock timeout lets a background process call SetForegroundWindow
    // directly; restore it afterwards.
    DWORD old = 0;
    SystemParametersInfoW(SPI_GETFOREGROUNDLOCKTIMEOUT, 0, &old, 0);
    SystemParametersInfoW(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, (PVOID)0, SPIF_SENDCHANGE);
    SetForegroundWindow(hwnd);
    BringWindowToTop(hwnd);
    SystemParametersInfoW(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, (PVOID)(UINT_PTR)old, SPIF_SENDCHANGE);
}
}
