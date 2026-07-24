#include "win_enumerator.hpp"
#include "window_filter.hpp"
#include <windows.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
namespace tsw {

static const char* B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string base64(const unsigned char* d, size_t n){
    std::string o; o.reserve((n+2)/3*4);
    for (size_t i=0;i<n;i+=3){
        unsigned v=d[i]<<16; int r=1;
        if(i+1<n){v|=d[i+1]<<8;r=2;} if(i+2<n){v|=d[i+2];r=3;}
        o+=B64[(v>>18)&63]; o+=B64[(v>>12)&63];
        o+=(r>1)?B64[(v>>6)&63]:'='; o+=(r>2)?B64[v&63]:'=';
    }
    return o;
}
static int pngEncoderClsid(CLSID* clsid){
    UINT num=0, size=0; Gdiplus::GetImageEncodersSize(&num,&size); if(!size) return -1;
    std::vector<unsigned char> buf(size);
    auto* codecs=reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num,size,codecs);
    for(UINT i=0;i<num;i++) if(wcscmp(codecs[i].MimeType,L"image/png")==0){ *clsid=codecs[i].Clsid; return (int)i; }
    return -1;
}
// Extract the window/app icon and return it as a base64 PNG ("" if none). GDI+ is lazily started.
static std::string iconPngBase64(HWND h){
    static std::once_flag once; static ULONG_PTR token=0;
    std::call_once(once, []{ Gdiplus::GdiplusStartupInput in; Gdiplus::GdiplusStartup(&token,&in,nullptr); });
    HICON ic=nullptr; DWORD_PTR r=0;
    SendMessageTimeoutW(h,WM_GETICON,ICON_SMALL2,0,SMTO_ABORTIFHUNG,150,&r); ic=(HICON)r;
    if(!ic){ SendMessageTimeoutW(h,WM_GETICON,ICON_BIG,0,SMTO_ABORTIFHUNG,150,&r); ic=(HICON)r; }
    if(!ic) ic=(HICON)GetClassLongPtrW(h,GCLP_HICONSM);
    if(!ic) ic=(HICON)GetClassLongPtrW(h,GCLP_HICON);
    if(!ic) return "";
    // Extract to a 32-bit BGRA buffer preserving transparency. Gdiplus::Bitmap(HICON) renders
    // transparent areas as opaque BLACK for many icons; instead read the color bitmap's alpha
    // directly, and for legacy icons with no alpha channel derive it from the AND mask.
    ICONINFO ii{}; if(!GetIconInfo(ic,&ii)) return "";
    BITMAP bm{}; GetObjectW(ii.hbmColor,sizeof bm,&bm);
    int iw=bm.bmWidth, ih=bm.bmHeight; std::string out;
    if(iw>0 && ih>0){
        BITMAPINFO bi{}; bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth=iw;
        bi.bmiHeader.biHeight=-ih; bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
        std::vector<unsigned char> px((size_t)iw*ih*4);
        HDC dc=GetDC(nullptr);
        GetDIBits(dc,ii.hbmColor,0,ih,px.data(),&bi,DIB_RGB_COLORS);
        bool hasAlpha=false; for(size_t i=3;i<px.size();i+=4) if(px[i]){ hasAlpha=true; break; }
        if(!hasAlpha){ std::vector<unsigned char> mk((size_t)iw*ih*4);
            if(GetDIBits(dc,ii.hbmMask,0,ih,mk.data(),&bi,DIB_RGB_COLORS))
                for(size_t i=0;i<px.size();i+=4) px[i+3]=mk[i]?0:255;   // mask set = transparent
            else for(size_t i=3;i<px.size();i+=4) px[i]=255;
        }
        ReleaseDC(nullptr,dc);
        Gdiplus::Bitmap bmp(iw,ih,iw*4,PixelFormat32bppARGB,px.data());
      if(bmp.GetLastStatus()==Gdiplus::Ok){
        CLSID png; if(pngEncoderClsid(&png)>=0){
            IStream* s=nullptr;
            if(CreateStreamOnHGlobal(nullptr,TRUE,&s)==S_OK){
                if(bmp.Save(s,&png,nullptr)==Gdiplus::Ok){
                    HGLOBAL hg=nullptr; GetHGlobalFromStream(s,&hg);
                    SIZE_T sz=GlobalSize(hg); void* p=GlobalLock(hg);
                    if(p&&sz) out=base64((const unsigned char*)p,sz);
                    GlobalUnlock(hg);
                }
                s->Release();
            }
        }
      }
    }
    if(ii.hbmColor) DeleteObject(ii.hbmColor);   // GetIconInfo allocates these; free to avoid a GDI leak
    if(ii.hbmMask)  DeleteObject(ii.hbmMask);
    return out;
}
static std::string procName(HWND h){
    DWORD pid=0; GetWindowThreadProcessId(h,&pid);
    HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    char buf[MAX_PATH]="";
    if(p){ DWORD n=MAX_PATH; QueryFullProcessImageNameA(p,0,buf,&n); CloseHandle(p); }
    std::string s=buf; auto pos=s.find_last_of("\\/"); return pos==std::string::npos?s:s.substr(pos+1);
}
static BOOL CALLBACK cb(HWND h, LPARAM lp){
    auto* out=reinterpret_cast<std::vector<WindowInfo>*>(lp);
    // Never list our own windows (e.g. the picker itself) — an agent's UI is not a task.
    DWORD pid=0; GetWindowThreadProcessId(h,&pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    RawWindow r;
    r.visible = IsWindowVisible(h);
    r.isRootOwner = (GetAncestor(h, GA_ROOTOWNER) == h);
    r.toolWindow = (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;
    int cloaked=0; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    r.cloaked = cloaked != 0;
    wchar_t title[512]=L""; GetWindowTextW(h, title, 512);
    char utf8[1024]=""; WideCharToMultiByte(CP_UTF8,0,title,-1,utf8,sizeof(utf8),nullptr,nullptr);
    r.title = utf8;
    if (isAltTabEligible(r)) {
        char idbuf[32]; sprintf_s(idbuf, "0x%llX", (unsigned long long)(uintptr_t)h);
        WindowInfo wi{ idbuf, r.title, procName(h), (int)pid };
        wi.iconPng = iconPngBase64(h);
        out->push_back(std::move(wi));
    }
    return TRUE;
}
// If the Windows Start menu is open on THIS machine, dismiss it. Called on every enumeration, so
// it runs on both the client (local, when the picker opens) and each server (session agent, on
// EnumRequest) — closing the Start menu uniformly, locally and inside RDP.
//
// The picker steals the foreground synthetically, which the shell does NOT treat as a real
// activation, so the Start menu never auto-dismisses. We dismiss it explicitly with Esc — but the
// Win key is typically still held from the invocation hotkey, and Win+Esc is a system shortcut that
// never reaches the Start menu. So if Win is down we first inject a Win key-up (flagged INJECTED, so
// the picker's own hook ignores it), turning the following Esc into a plain Esc that light-dismisses.
static void hideStartMenu(){
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return;
    wchar_t path[MAX_PATH] = L""; DWORD n = MAX_PATH; QueryFullProcessImageNameW(p, 0, path, &n); CloseHandle(p);
    std::wstring s = path; auto pos = s.find_last_of(L"\\/");
    std::wstring exe = pos == std::wstring::npos ? s : s.substr(pos + 1);
    // Start-menu host varies by Windows build: classic Start is StartMenuExperienceHost, but on
    // Win11 (unified Start+Search) the focused surface is SearchHost. Either being the foreground
    // means the Start/Search overlay is up.
    if (_wcsicmp(exe.c_str(), L"StartMenuExperienceHost.exe") && _wcsicmp(exe.c_str(), L"SearchHost.exe")) return;
    INPUT in[3] = {}; int i = 0;
    bool winDown = ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
    if (winDown) { in[i].type = INPUT_KEYBOARD; in[i].ki.wVk = VK_LWIN; in[i].ki.dwFlags = KEYEVENTF_KEYUP; i++; }
    in[i].type = INPUT_KEYBOARD; in[i].ki.wVk = VK_ESCAPE; i++;
    in[i].type = INPUT_KEYBOARD; in[i].ki.wVk = VK_ESCAPE; in[i].ki.dwFlags = KEYEVENTF_KEYUP; i++;
    SendInput(i, in, sizeof(INPUT));
}
std::vector<WindowInfo> enumerateWindows(){
    hideStartMenu();
    std::vector<WindowInfo> v;
    // Synthetic per-machine entry: focus THIS computer, minimize everything, show its desktop.
    v.push_back({ "desktop", "Desktop", "", 0 });
    EnumWindows(cb,(LPARAM)&v);
    return v;
}
}
