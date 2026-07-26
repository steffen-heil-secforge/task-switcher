// Acrylic task picker (Claude Design mockup). Centered DWM-Acrylic panel, tasks grouped by
// computer with per-machine accent colors, real app icons (falling back to an accent initial
// badge), light/dark themes, Direct2D/DirectWrite. Per-pixel alpha via a layered window +
// UpdateLayeredWindow, D2D rendering through an ID2D1DCRenderTarget bound to a DIB.
//
// Interaction (confirmed): the existing global hotkey opens it (client LL hook, or the server's
// Hotkey relay when focus was in RDP); once open the picker owns focus and the LL hook drives
// navigation — Tab/↓→ forward, Shift+Tab/↑← back, Ctrl+` next computer, Enter commit, Esc cancel,
// plus mouse hover/click.
#define NOMINMAX   // let std::min/std::max work (windows.h otherwise defines min/max macros)
#include <windows.h>
#include <windowsx.h>
#include <shellscalingapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <map>
#include <cstdio>
#include <cmath>
#include <cwctype>
#include <algorithm>
#include <utility>
#include "hub.hpp"
#include "logo_png.h"
#include <thread>
#include <atomic>
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "ole32.lib")

namespace tsw {

// ---- undocumented acrylic (SetWindowCompositionAttribute) ----
enum ACCENT_STATE { ACCENT_ENABLE_ACRYLICBLURBEHIND = 4 };
struct ACCENT_POLICY { int AccentState, AccentFlags; unsigned GradientColor; int AnimationId; };
struct WINCOMPATTRDATA { int Attrib; void* pvData; size_t cbData; };
typedef BOOL (WINAPI* PSWCA)(HWND, WINCOMPATTRDATA*);

static Hub* g_hub = nullptr;
static HWND g_win = nullptr;
static HHOOK g_kbHook = nullptr;
static std::atomic<bool> g_visible{false};   // read from the dedicated hook thread + the RDP relay
static std::atomic<bool> g_ctrlTab{false};   // Ctrl+^ was used as a Tab (while open) -> releasing Ctrl commits
static ULONGLONG g_shownAt = 0;   // guards close-on-blur against the activation race right after show
static int  g_fgTries = 0;        // foreground-steal retry count
static DWORD g_lockOld = 0; static bool g_lockChanged = false;

// Force our window to the foreground from a background process (the hotkey's input went to mstsc,
// not us). Attach to the current foreground thread's input queue so SetForegroundWindow is allowed.
static bool forceForeground(HWND h){
    HWND fg=GetForegroundWindow(); DWORD tc=GetCurrentThreadId(), tf=fg?GetWindowThreadProcessId(fg,nullptr):0;
    if(tf&&tf!=tc) AttachThreadInput(tc,tf,TRUE);
    SetWindowPos(h,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
    SetForegroundWindow(h); BringWindowToTop(h); SetActiveWindow(h); SetFocus(h);
    if(tf&&tf!=tc) AttachThreadInput(tc,tf,FALSE);
    return GetForegroundWindow()==h;
}
static bool g_darkOverride = false, g_hasOverride = false;   // header toggle; else follow OS
#define WM_TSW_SHOW (WM_APP + 1)
#define WM_TSW_KEY  (WM_APP + 2)
#define WM_TSW_LOCAL_SHOW (WM_APP + 3)
#define WM_TSW_REMOTE_SHOW (WM_APP + 4)

struct RemotePreselection {
    std::string endpoint;
    std::string hwnd;
};

// ---- D2D / DWrite / WIC singletons ----
static ID2D1Factory*       g_d2d = nullptr;
static IDWriteFactory*     g_dw  = nullptr;
static IWICImagingFactory* g_wic = nullptr;
static IDWriteTextFormat *g_fTitle=0,*g_fSub=0,*g_fGroup=0,*g_fTile=0,*g_fBadge=0,*g_fKbd=0;

static IDWriteTextFormat* mkFmt(float size, DWRITE_FONT_WEIGHT w){
    IDWriteTextFormat* f=nullptr;
    g_dw->CreateTextFormat(L"Segoe UI Variable Text", nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                           DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
    if(!f) g_dw->CreateTextFormat(L"Segoe UI", nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
    return f;
}
static void initGfx(){
    if(g_d2d) return;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    // sizes are DIP; we scale by DPI at layout time by rebuilding formats per-show
}

// ---- theme ----
struct Palette {
    D2D1_COLOR_F backdropTint; float panelTintA;
    D2D1_COLOR_F panel, panelBorder, text, subtle, tile, tileBorder, tileText, line, kbdBorder, kbdText, segBg, segOn, segText, shadow;
};
static D2D1_COLOR_F rgb(unsigned hex, float a=1){ return D2D1::ColorF((hex>>16&255)/255.f,(hex>>8&255)/255.f,(hex&255)/255.f,a); }
static bool osDark(){
    DWORD v=1, n=sizeof v; HKEY k;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",0,KEY_READ,&k)==0){
        RegQueryValueExW(k,L"AppsUseLightTheme",0,0,(LPBYTE)&v,&n); RegCloseKey(k);
    }
    return v==0;
}
static bool useDark(){ return g_hasOverride ? g_darkOverride : osDark(); }
static Palette palette(bool dark){
    Palette p{};
    if(dark){
        p.panelTintA=0.34f; p.panel=rgb(0x1A1B20); p.panelBorder=rgb(0xFFFFFF,0.09f); p.text=rgb(0xEDEDED);
        p.subtle=rgb(0xFFFFFF,0.42f); p.tile=rgb(0x33343B); p.tileBorder=rgb(0xFFFFFF,0.08f); p.tileText=rgb(0xFFFFFF,0.88f);
        p.line=rgb(0xFFFFFF,0.08f); p.kbdBorder=rgb(0xFFFFFF,0.20f); p.kbdText=rgb(0xFFFFFF,0.60f);
        p.segBg=rgb(0xFFFFFF,0.06f); p.segOn=rgb(0xFFFFFF,0.14f); p.segText=rgb(0xFFFFFF,0.55f); p.shadow=rgb(0x000000,0.25f);
    } else {
        p.panelTintA=0.42f; p.panel=rgb(0xFFFFFF); p.panelBorder=rgb(0xFFFFFF,0.60f); p.text=rgb(0x1C1C1E);
        p.subtle=rgb(0x000000,0.42f); p.tile=rgb(0xDFE3EA); p.tileBorder=rgb(0x000000,0.09f); p.tileText=rgb(0x2B2C31);
        p.line=rgb(0x000000,0.08f); p.kbdBorder=rgb(0x000000,0.20f); p.kbdText=rgb(0x000000,0.50f);
        p.segBg=rgb(0x000000,0.05f); p.segOn=rgb(0xFFFFFF); p.segText=rgb(0x000000,0.50f); p.shadow=rgb(0x141A2D,0.08f);
    }
    return p;
}
// selection accent (both themes)
static D2D1_COLOR_F ACC_FILL(){ return rgb(0x3F63E0); }
static D2D1_COLOR_F ACC_BORDER(){ return rgb(0x7091F0); }
// per-machine accent palette (extend by hue rotation past 3)
static D2D1_COLOR_F machineAccent(int i){
    static const unsigned base[]={0x4E78DE,0x1F9E6B,0xC7883C};
    if(i<3) return rgb(base[i]);
    // rotate hue for extras (simple HSV wheel)
    float h = fmodf(70.f + (i-2)*47.f, 360.f)/360.f, s=0.55f, v=0.80f;
    float r,g,b; int hi=(int)(h*6); float f=h*6-hi, pv=v*(1-s), q=v*(1-f*s), t=v*(1-(1-f)*s);
    switch(hi%6){case 0:r=v;g=t;b=pv;break;case 1:r=q;g=v;b=pv;break;case 2:r=pv;g=v;b=t;break;
                 case 3:r=pv;g=q;b=v;break;case 4:r=t;g=pv;b=v;break;default:r=v;g=pv;b=q;}
    return D2D1::ColorF(r,g,b);
}

// ---- model + layout ----
struct Tile { Entry e; std::wstring title; wchar_t initial; D2D1_RECT_F rc; int flat; float nw; };
struct Group { std::wstring name; D2D1_COLOR_F accent; int count; D2D1_RECT_F headRc; };
static std::vector<Tile>  g_tiles;
static std::vector<Group> g_groups;
static int  g_sel = 0;
static float g_scroll = 0, g_contentH = 0, g_areaTop = 0, g_areaBot = 0;
static float g_scale = 1;
static std::string g_preEp, g_preHwnd; static bool g_preApplied = true;

// Device-independent decoded icon pixels (premultiplied BGRA). Decoded from PNG ONCE per unique
// icon and cached for the picker's lifetime, so re-rendering (every keystroke / the fill timer)
// is just a cheap CreateBitmap from memory instead of a full WIC PNG decode each time. This is
// what makes navigation smooth even with many remote windows.
struct IconPix { UINT w=0, h=0; std::vector<BYTE> px; };
static std::map<std::string, IconPix> g_iconCache;
static const IconPix* iconPixFor(const std::string& b64){
    if(b64.empty() || !g_wic) return nullptr;
    auto it = g_iconCache.find(b64);
    if(it != g_iconCache.end()) return it->second.w ? &it->second : nullptr;
    IconPix ip;
    static const std::string T="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<unsigned char> bytes; int val=0,bits=0;
    for(char c:b64){ if(c=='=')break; auto pos=T.find(c); if(pos==std::string::npos)continue; val=(val<<6)|(int)pos; bits+=6; if(bits>=8){bits-=8; bytes.push_back((unsigned char)((val>>bits)&255));}}
    if(!bytes.empty()){
        IWICStream* st=nullptr; g_wic->CreateStream(&st);
        if(st){
            st->InitializeFromMemory(bytes.data(),(DWORD)bytes.size());
            IWICBitmapDecoder* dec=nullptr; g_wic->CreateDecoderFromStream(st,nullptr,WICDecodeMetadataCacheOnLoad,&dec);
            if(dec){ IWICBitmapFrameDecode* fr=nullptr; dec->GetFrame(0,&fr);
                if(fr){ IWICFormatConverter* cv=nullptr; g_wic->CreateFormatConverter(&cv);
                    if(cv){ cv->Initialize(fr,GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
                        cv->GetSize(&ip.w,&ip.h);
                        if(ip.w && ip.h){ ip.px.resize((size_t)ip.w*ip.h*4); cv->CopyPixels(nullptr, ip.w*4, (UINT)ip.px.size(), ip.px.data()); }
                        cv->Release(); }
                    fr->Release(); }
                dec->Release(); }
            st->Release();
        }
    }
    auto& slot = (g_iconCache[b64] = std::move(ip));
    return slot.w ? &slot : nullptr;
}

// The app logo (embedded transparent PNG, shown in the header's upper-right). Decoded to
// premultiplied BGRA once — like the icons — then drawn via a cheap CreateBitmap each render.
static const IconPix* logoPix(bool dark){
    static IconPix ipLight, ipDark; static bool triedLight=false, triedDark=false;
    IconPix& ip = dark ? ipDark : ipLight;
    bool& tried = dark ? triedDark : triedLight;
    const unsigned char* png = dark ? kLogoDark : kLogoLight;
    unsigned pngLen = dark ? kLogoDarkLen : kLogoLightLen;
    if(tried) return ip.w ? &ip : nullptr;
    tried=true;
    if(!g_wic) return nullptr;
    IWICStream* st=nullptr; g_wic->CreateStream(&st);
    if(st){
        st->InitializeFromMemory(const_cast<BYTE*>(png),(DWORD)pngLen);
        IWICBitmapDecoder* dec=nullptr; g_wic->CreateDecoderFromStream(st,nullptr,WICDecodeMetadataCacheOnLoad,&dec);
        if(dec){ IWICBitmapFrameDecode* fr=nullptr; dec->GetFrame(0,&fr);
            if(fr){ IWICFormatConverter* cv=nullptr; g_wic->CreateFormatConverter(&cv);
                if(cv){ cv->Initialize(fr,GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
                    cv->GetSize(&ip.w,&ip.h);
                    if(ip.w && ip.h){ ip.px.resize((size_t)ip.w*ip.h*4); cv->CopyPixels(nullptr, ip.w*4, (UINT)ip.px.size(), ip.px.data()); }
                    cv->Release(); }
                fr->Release(); }
            dec->Release(); }
        st->Release();
    }
    return ip.w ? &ip : nullptr;
}

static float measureText(const std::wstring& s, IDWriteTextFormat* f, float maxW){
    IDWriteTextLayout* l=nullptr;
    g_dw->CreateTextLayout(s.c_str(),(UINT32)s.size(),f,maxW,1000,&l);
    DWRITE_TEXT_METRICS m{}; if(l){ l->GetMetrics(&m); l->Release(); }
    return m.widthIncludingTrailingWhitespace;
}

// Build tiles/groups and compute geometry. Returns panel (w,h) in px.
static float footerH(){ return (18+8+16+12)*g_scale; }   // gap-above + line-to-chips + chip-h + gap-below
static float tileHeight(){ return (7+7+19)*g_scale; }   // padding 7*2 + 19 icon

// Phase 1: build the tile/group model and each tile's natural width (independent of panel width).
static void buildTiles(ID2D1RenderTarget* rt){
    float S=g_scale;
    auto rows = g_hub->snapshot();
    g_tiles.clear(); g_groups.clear();
    std::string curEp="\x01";
    std::vector<std::pair<std::string,std::vector<Entry>>> byEp;
    for(auto& e: rows){ if(e.endpointId!=curEp){ byEp.push_back({e.endpointId,{}}); curEp=e.endpointId; } byEp.back().second.push_back(e); }
    int flat=0; float maxTileW=196*S, iconSlot=(8+19+8)*S, padR=12*S;
    for(size_t m=0;m<byEp.size();++m){
        Group g; g.accent=machineAccent((int)m);
        wchar_t wn[256]; MultiByteToWideChar(CP_UTF8,0,byEp[m].first.c_str(),-1,wn,256); g.name=wn;
        g.count=(int)byEp[m].second.size(); g_groups.push_back(g);
        for(auto& e: byEp[m].second){
            Tile t; t.e=e; t.flat=flat++;
            wchar_t wt[600]; MultiByteToWideChar(CP_UTF8,0,e.w.title.c_str(),-1,wt,600); t.title=wt;
            std::wstring app=t.title; size_t dash=app.rfind(L" — "); if(dash!=std::wstring::npos) app=app.substr(dash+3);
            t.initial = app.empty()? L'?' : towupper(app[0]);
            float tw=measureText(t.title, g_fTile, maxTileW);
            t.nw = iconSlot + tw + padR; if(t.nw>maxTileW) t.nw=maxTileW;
            t.rc = D2D1::RectF(0,0,0,0);
            g_tiles.push_back(std::move(t));
        }
    }
}
// Phase 2: wrap the tiles into a panel of width W; set positions + g_areaTop/g_contentH.
// Returns the total wanted panel height (content + footer). Called repeatedly to pick a width.
static const float kTileIndent = 18.f;   // tasks sit indented under their machine header (DIP)
static float arrange(float W){
    float S=g_scale, padX=30*S, gap=7*S, tileH=tileHeight();
    float startX=padX + kTileIndent*S, rightX=W-padX;   // tiles indented; wrap within [startX, rightX]
    float y=66.f*S; g_areaTop=y;   // header now fits the larger title + the (doubled) logo
    size_t idx=0;
    for(size_t gi=0; gi<g_groups.size(); ++gi){
        auto& g=g_groups[gi];
        y += (gi==0 ? 12.f*S : 24.f*S);   // extra breathing room before each subsequent machine
        g.headRc=D2D1::RectF(padX,y,rightX,y+14*S); y+=14*S+9*S;
        float x=startX;
        for(int k=0;k<g.count;k++,idx++){
            float nw=g_tiles[idx].nw;
            if(x>startX && x+nw>rightX){ x=startX; y+=tileH+gap; }   // wrap
            g_tiles[idx].rc=D2D1::RectF(x,y,x+nw,y+tileH);
            x+=nw+gap;
        }
        y+=tileH;
    }
    y+=4*S; g_contentH=y;
    return g_contentH + footerH();
}
// Choose a panel width "by need": as wide as the content wants (widest group in one row), capped
// at 60% of the work area; widen further (up to 90%) only if that keeps the panel from exceeding
// maxH (i.e. to avoid scrolling). Returns W; sets pw via arrange() side effects.
static float chooseWidth(float monW, float maxH){
    float S=g_scale, padX=30*S;
    float widest=0; for(auto& t: g_tiles) widest=std::max(widest,t.nw);
    float minW=std::max(widest + 2*padX + kTileIndent*S, 360*S);   // tiles are indented under the header
    // Cap width so it never sprawls on a big/4K monitor: <=45% of the work area, and never
    // more than ~1100 DIP. (Height is free to grow, and scrolls past the max.)
    float capW=std::min(0.45f*monW, 1100*S);
    if(capW<minW) capW=minW;
    // Aim for a 3:2 (W:H) panel: sweep candidate widths and pick the one whose width is closest
    // to 1.5x its height. Widening cuts height (more tiles per row); a small penalty on overflow
    // nudges toward not scrolling.
    float bestW=minW, best=1e30f;
    for(float W=minW; W<=capW+1; W+=40*S){
        float wanted=arrange(W);
        float h=std::min(wanted,maxH);
        float score=std::fabs(W - 1.5f*h) + (wanted>maxH ? (wanted-maxH)*0.5f : 0.f);
        if(score<best){ best=score; bestW=W; }
    }
    arrange(bestW);
    return bestW;
}

static void drawRound(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, ID2D1Brush* fill, ID2D1Brush* border=nullptr, float bw=1){
    D2D1_ROUNDED_RECT rr=D2D1::RoundedRect(r,rad,rad);
    if(fill) rt->FillRoundedRectangle(rr,fill);
    if(border) rt->DrawRoundedRectangle(rr,border,bw);
}

static void render();
static D2D1_COLOR_F machineAccentFor(size_t tileIdx);   // group accent for a flat tile index

// ---- window / hooks ----
static int hitTile(int px,int py){
    for(size_t i=0;i<g_tiles.size();++i){ auto r=g_tiles[i].rc; float yo=r.top-g_scroll, yb=r.bottom-g_scroll;
        if(yo>=g_areaTop-1 && yb<=g_areaBot+1 && px>=r.left && px<=r.right && py>=yo && py<=yb) return (int)i; }
    return -1;
}
static int findRow(const std::string& ep, const std::string& hwnd){
    if(ep.empty()&&hwnd.empty()) return -1;
    for(size_t i=0;i<g_tiles.size();++i) if(g_tiles[i].e.endpointId==ep && g_tiles[i].e.w.hwnd==hwnd) return (int)i;
    return -1;
}
static float maxScroll(){ float m = g_contentH - g_areaBot; return m > 0 ? m : 0; }   // 0 when it all fits
static void ensureVisible(){
    if(g_sel<0||g_sel>=(int)g_tiles.size()) return;
    auto r=g_tiles[g_sel].rc;
    float viewTop=g_areaTop+g_scroll, viewBot=g_areaBot+g_scroll;
    if(r.top<viewTop) g_scroll=r.top-g_areaTop;
    else if(r.bottom>viewBot) g_scroll=r.bottom-g_areaBot;
    float ms=maxScroll(); if(g_scroll>ms)g_scroll=ms; if(g_scroll<0)g_scroll=0;
}
static void restoreLock(){ if(g_lockChanged){ SystemParametersInfoW(SPI_SETFOREGROUNDLOCKTIMEOUT,0,(PVOID)(UINT_PTR)g_lockOld,SPIF_SENDCHANGE); g_lockChanged=false; } }
static void commitSel(){
    g_visible=false; ShowWindow(g_win,SW_HIDE); restoreLock();
    if(g_sel>=0 && g_sel<(int)g_tiles.size()){ Entry e=g_tiles[g_sel].e; g_hub->activate(e); }
}
static void cancel(){ g_visible=false; ShowWindow(g_win,SW_HIDE); restoreLock(); }   // flag first so WM_ACTIVATE can't re-enter
static void navForward(int d){ int n=(int)g_tiles.size(); if(!n)return; g_preApplied=true; g_sel=((g_sel+d)%n+n)%n; ensureVisible(); render(); }
// Spatial up/down: move to the tile roughly directly above/below the current one (nearest row,
// then closest horizontally). dir = +1 down, -1 up.
static void navSpatial(int dir){
    int n=(int)g_tiles.size(); if(!n)return;
    if(g_sel<0){ navForward(1); return; }
    auto& cr=g_tiles[g_sel].rc; float ccx=(cr.left+cr.right)/2, ccy=(cr.top+cr.bottom)/2;
    int best=-1; float bestScore=1e30f;
    for(int i=0;i<n;i++){ if(i==g_sel) continue;
        auto& r=g_tiles[i].rc; float cx=(r.left+r.right)/2, cy=(r.top+r.bottom)/2, dy=cy-ccy;
        if(dir>0 && dy<=1) continue;         // want strictly below
        if(dir<0 && dy>=-1) continue;        // want strictly above
        float score=std::fabs(dy)*3.f + std::fabs(cx-ccx);   // nearest row first, then horizontal
        if(score<bestScore){ bestScore=score; best=i; }
    }
    if(best>=0){ g_preApplied=true; g_sel=best; ensureVisible(); render(); }
}
// first tile of the current group's neighbour (dir = +1 next computer, -1 previous)
static void gotoComputer(int dir){
    int n=(int)g_tiles.size(); int ng=(int)g_groups.size(); if(!n||!ng)return;
    int gi=0,acc=0; for(int i=0;i<ng;++i){ if(g_sel<acc+g_groups[i].count){gi=i;break;} acc+=g_groups[i].count; }
    int tg=((gi+dir)%ng+ng)%ng; int first=0; for(int i=0;i<tg;i++) first+=g_groups[i].count;
    g_preApplied=true; g_sel=first%n; ensureVisible(); render();
}

static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM wp,LPARAM lp){
    switch(m){
    case WM_TSW_REMOTE_SHOW: {
        RemotePreselection* pre=(RemotePreselection*)lp;
        if(pre){
            g_preEp=std::move(pre->endpoint);
            g_preHwnd=std::move(pre->hwnd);
            delete pre;
        }
        g_preApplied=false;
        PostMessageW(h,WM_TSW_SHOW,0,0);
        return 0; }
    case WM_TSW_LOCAL_SHOW: {
        // Keep the preselection model UI-thread-owned. The hook thread only captures the
        // foreground HWND and delivers it here through the window's message queue.
        HWND fg=(HWND)lp;
        wchar_t cls[64]=L""; if(fg) GetClassNameW(fg,cls,64);
        bool onDesktop = !lstrcmpW(cls,L"Progman") || !lstrcmpW(cls,L"WorkerW");
        char b[32]; sprintf_s(b,"0x%llX",(unsigned long long)(uintptr_t)fg);
        g_preEp=g_hub->localName();
        g_preHwnd = onDesktop ? std::string("desktop") : std::string(b);
        g_preApplied=false;
        logLine(std::string("picker: local hook fired (Ctrl+^), pre=")+g_preHwnd);
        PostMessageW(h,WM_TSW_SHOW,0,0);
        return 0; }
    case WM_TSW_SHOW: {
        logLine("picker: WM_TSW_SHOW");
        g_hub->refreshLocal(); g_hub->requestEnum();
        g_sel=-1; g_scroll=0; g_visible=true; g_ctrlTab=false;   // -1: nothing focused until the preselect target resolves
        render();  // sizes + positions + presents the layered window (shows it); reconciles selection
        // Disable the foreground-lock timeout so the steal is permitted (the hotkey's input went to
        // mstsc, not us), then keep re-asserting foreground for a few hundred ms via timer 2 until
        // it actually sticks. This is what the manual "second press" was doing over RDP.
        if(!g_lockChanged){ SystemParametersInfoW(SPI_GETFOREGROUNDLOCKTIMEOUT,0,&g_lockOld,0); g_lockChanged=true; }
        SystemParametersInfoW(SPI_SETFOREGROUNDLOCKTIMEOUT,0,(PVOID)0,SPIF_SENDCHANGE);
        bool ok=forceForeground(h);
        logLine(std::string("picker: show, foreground ok=")+std::to_string(ok));
        g_shownAt=GetTickCount64(); g_fgTries=0;
        SetTimer(h,1,250,nullptr);   // incremental fill as slow servers reply
        if(!ok) SetTimer(h,2,35,nullptr);   // retry the steal until it takes
        return 0; }
    case WM_TIMER:
        if(wp==2){   // foreground-steal retry (over RDP the first attempt often loses to mstsc)
            if(!g_visible || forceForeground(h) || ++g_fgTries>10){ KillTimer(h,2); logLine("picker: fg retry done tries="+std::to_string(g_fgTries)); }
            return 0;
        }
        if(wp==1){ if(!g_visible){ KillTimer(h,1); return 0; }
            render();   // re-snapshots + reconciles selection (applies the preselect once it arrives)
        }
        return 0;
    case WM_TSW_KEY: {
        int vk=(int)wp; bool shift=(lp&1)!=0, ctrl=(lp&2)!=0;
        if(vk==VK_ESCAPE){ cancel(); return 0; }
        if(!g_preApplied){   // still waiting for the preselect target: the first key takes over -> focus the first item
            g_preApplied=true; g_sel = g_tiles.empty()? -1 : 0; ensureVisible(); render(); return 0;
        }
        if(vk==VK_TAB||vk==VK_OEM_3) gotoComputer(shift?-1:1);   // Tab / backtick: computer-to-computer
        else if(vk==VK_DOWN) navSpatial(1);
        else if(vk==VK_UP)   navSpatial(-1);
        else if(vk==VK_RIGHT) navForward(1);
        else if(vk==VK_LEFT)  navForward(-1);
        else if(vk==VK_RETURN) commitSel();
        return 0; }
    case WM_MOUSEMOVE: { int i=hitTile(GET_X_LPARAM(lp),GET_Y_LPARAM(lp)); if(i>=0&&i!=g_sel){g_preApplied=true;g_sel=i;render();} return 0; }
    case WM_LBUTTONUP: { int i=hitTile(GET_X_LPARAM(lp),GET_Y_LPARAM(lp)); if(i>=0){g_preApplied=true;g_sel=i;commitSel();} return 0; }
    case WM_MOUSEWHEEL: { g_scroll -= (float)GET_WHEEL_DELTA_WPARAM(wp)/120.f*40*g_scale; float ms=maxScroll(); if(g_scroll>ms)g_scroll=ms; if(g_scroll<0)g_scroll=0; render(); return 0; }
    case WM_ACTIVATE: if(LOWORD(wp)==WA_INACTIVE && g_visible && GetTickCount64()-g_shownAt>400) cancel(); return 0;   // lost focus -> close (ignore the show-time race)
    case WM_CLOSE: cancel(); return 0;
    }
    return DefWindowProcW(h,m,wp,lp);
}

// The invocation key is the top-left "^" key (scancode 0x29 on any layout) with Ctrl held.
static const DWORD kHotkeyScan = 0x29;
// Low-level keyboard hook: opens on Ctrl+^; while visible, drives navigation. Ctrl (not the
// Windows key) is the modifier so the hotkey has no OS side effects (Start / Win+L / Win+R) and a
// briefly-lingering modifier across the picker's focus switch is harmless.
static LRESULT CALLBACK kbProc(int code, WPARAM wp, LPARAM lp){
    if(code==HC_ACTION && (wp==WM_KEYUP||wp==WM_SYSKEYUP)){
        auto* k=(KBDLLHOOKSTRUCT*)lp;
        // Releasing Ctrl after using Ctrl+^ as a Tab commits the selection (native switcher feel).
        if(!(k->flags & LLKHF_INJECTED) && (k->vkCode==VK_LCONTROL||k->vkCode==VK_RCONTROL)){
            if(g_visible && g_ctrlTab){ g_ctrlTab=false; PostMessageW(g_win,WM_TSW_KEY,VK_RETURN,0); }   // release-Ctrl = Enter
        }
    }
    if(code==HC_ACTION && (wp==WM_KEYDOWN||wp==WM_SYSKEYDOWN)){
        auto* k=(KBDLLHOOKSTRUCT*)lp;
        if(k->flags & LLKHF_INJECTED) return CallNextHookEx(g_kbHook,code,wp,lp);   // ignore injected input
        bool ctrl=(GetAsyncKeyState(VK_CONTROL)&0x8000)!=0, shift=(GetAsyncKeyState(VK_SHIFT)&0x8000)!=0;
        if(k->scanCode==kHotkeyScan){
            // While the picker is open, "^" cycles like Tab. (Opened from inside an RDP session,
            // mstsc may have consumed the local Ctrl, so GetAsyncKeyState(VK_CONTROL) can read up
            // even though it's physically held — cycle regardless.)
            if(g_visible){ g_ctrlTab=true; PostMessageW(g_win,WM_TSW_KEY,(shift?VK_LEFT:VK_RIGHT),0); return 1; }   // Ctrl+^ = next task (->)
            if(!ctrl) return CallNextHookEx(g_kbHook,code,wp,lp);   // bare ^ while hidden = normal dead key
            HWND fg=GetForegroundWindow();
            if(g_win) PostMessageW(g_win,WM_TSW_LOCAL_SHOW,0,(LPARAM)fg);
            return 1;
        }
        if(g_visible){
            int vk=(int)k->vkCode;
            if(vk==VK_TAB||vk==VK_UP||vk==VK_DOWN||vk==VK_LEFT||vk==VK_RIGHT||vk==VK_OEM_3||vk==VK_RETURN||vk==VK_ESCAPE){
                PostMessageW(g_win,WM_TSW_KEY,vk,(shift?1:0)|(ctrl?2:0));
                return 1;   // swallow
            }
        }
    }
    return CallNextHookEx(g_kbHook,code,wp,lp);
}

// ---- rendering to a DIB + UpdateLayeredWindow ----
static void render(){
    if(!g_win||!g_visible) return;
    initGfx();
    // DPI of the monitor under the cursor
    POINT cur; GetCursorPos(&cur);
    HMONITOR mon=MonitorFromPoint(cur,MONITOR_DEFAULTTONEAREST);
    UINT dx=96,dy=96; GetDpiForMonitor(mon,MDT_EFFECTIVE_DPI,&dx,&dy); g_scale=dx/96.f;
    // (re)build scaled text formats
    auto rel=[&](IDWriteTextFormat*& f,float sz,DWRITE_FONT_WEIGHT w){ if(f)f->Release(); f=mkFmt(sz*g_scale,w); };
    rel(g_fTitle,18,DWRITE_FONT_WEIGHT_SEMI_BOLD); rel(g_fSub,13,DWRITE_FONT_WEIGHT_NORMAL);
    rel(g_fGroup,14,DWRITE_FONT_WEIGHT_SEMI_BOLD); rel(g_fTile,14.5f,DWRITE_FONT_WEIGHT_NORMAL);
    rel(g_fBadge,11,DWRITE_FONT_WEIGHT_BOLD); rel(g_fKbd,12,DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_fTile->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g_fTitle->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_fBadge->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); g_fBadge->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    MONITORINFO mi{sizeof mi}; GetMonitorInfo(mon,&mi);
    float monW=(float)(mi.rcWork.right-mi.rcWork.left);
    float monH=(float)(mi.rcWork.bottom-mi.rcWork.top);
    float maxPanelH=monH*0.86f;   // panel fits content; only scrolls if it would exceed this

    // We need a render target to decode icons + measure; create a DC render target on a DIB.
    float pw,ph;
    HDC screen=GetDC(nullptr); HDC memdc=CreateCompatibleDC(screen);
    // scratch canvas sized to the largest panel we might produce (90% work-area); we present only pw x ph
    int gw=(int)(monW*0.9f)+1, gh=(int)maxPanelH+1;
    BITMAPINFO bi{}; bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); bi.bmiHeader.biWidth=gw; bi.bmiHeader.biHeight=-gh; bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr; HBITMAP dib=CreateDIBSection(screen,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
    HGDIOBJ old=SelectObject(memdc,dib);
    ID2D1DCRenderTarget* rt=nullptr;
    D2D1_RENDER_TARGET_PROPERTIES rp=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    g_d2d->CreateDCRenderTarget(&rp,&rt);
    RECT full={0,0,gw,gh}; rt->BindDC(memdc,&full);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    // Capture the user's current selection identity BEFORE the rebuild (indices shift as rows fill).
    std::string keepEp, keepHwnd; bool keep = g_preApplied && g_sel>=0 && g_sel<(int)g_tiles.size();
    if(keep){ keepEp=g_tiles[g_sel].e.endpointId; keepHwnd=g_tiles[g_sel].e.w.hwnd; }
    buildTiles(rt);                              // model + per-tile natural widths (icons decoded on rt)
    pw = chooseWidth(monW, maxPanelH);           // dynamic width; arranges tiles for the chosen width
    ph = std::min(g_contentH + footerH(), maxPanelH);
    g_areaBot = ph - footerH();
    // Selection reconciliation:
    //  - preselect not yet applied: focus the previously-focused window if it's present; if it
    //    isn't there yet, focus NOTHING (g_sel=-1) until it appears (a later render) or a key is
    //    pressed (WM_TSW_KEY focuses the first item). Avoids highlighting the wrong row on open.
    //  - preselect applied (user owns the selection): keep the same row by identity across rebuilds.
    if(!g_preApplied){
        int idx=findRow(g_preEp,g_preHwnd);
        logLine("reconcile pre=["+g_preEp+"|"+g_preHwnd+"] found="+std::to_string(idx));
        g_sel=idx;
        if(idx>=0) g_preApplied=true;
    } else if(keep){
        int idx=findRow(keepEp,keepHwnd);
        g_sel = (idx>=0) ? idx : (g_tiles.empty()? -1 : 0);
    } else if(g_sel>=(int)g_tiles.size()){
        g_sel = g_tiles.empty()? -1 : 0;
    }
    ensureVisible();

    int W=(int)pw, H=(int)ph;
    Palette P=palette(useDark());

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0,0,0,0));   // transparent -> acrylic/desktop shows through
    ID2D1SolidColorBrush* br=nullptr; rt->CreateSolidColorBrush(D2D1::ColorF(0,0,0),&br);
    auto set=[&](D2D1_COLOR_F c){ br->SetColor(c); return (ID2D1Brush*)br; };
    D2D1_RECT_F panel=D2D1::RectF(0,0,(float)W,(float)H); float rad=12*g_scale;
    // panel tint (self-drawn; acrylic blur, if it composites, sits behind this)
    D2D1_COLOR_F pt=P.panel; pt.a=P.panelTintA; drawRound(rt,panel,rad,set(pt));
    drawRound(rt,D2D1::RectF(0.5f,0.5f,W-0.5f,H-0.5f),rad,nullptr,set(P.panelBorder),1*g_scale);

    float padX=30*g_scale;
    // header
    std::wstring title=L"Task Switcher";
    rt->DrawText(title.c_str(),(UINT32)title.size(),g_fTitle,D2D1::RectF(padX,20*g_scale,W-padX,50*g_scale),set(P.text));
    float tw=measureText(title,g_fTitle,400*g_scale);
    wchar_t sub[128]; swprintf(sub,128,L"\u00B7 %d computers \u00B7 %d open",(int)g_groups.size(),(int)g_tiles.size());
    rt->DrawText(sub,(UINT32)wcslen(sub),g_fSub,D2D1::RectF(padX+tw+8*g_scale,26*g_scale,W-padX,50*g_scale),set(P.subtle));

    // logo, upper-right (transparent PNG, aspect-preserved)
    if(const IconPix* lp=logoPix(useDark())){
        float lh=44*g_scale, lw=lh*(float)lp->w/(float)lp->h;
        D2D1_RECT_F lr=D2D1::RectF(W-padX-lw, 10*g_scale, W-padX, 10*g_scale+lh);
        ID2D1Bitmap* lb=nullptr;
        D2D1_BITMAP_PROPERTIES bp=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        rt->CreateBitmap(D2D1::SizeU(lp->w,lp->h), lp->px.data(), lp->w*4, bp, &lb);
        if(lb){ rt->DrawBitmap(lb,lr,1.0f,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR); lb->Release(); }
    }

    // clip task area
    rt->PushAxisAlignedClip(D2D1::RectF(0,g_areaTop,(float)W,g_areaBot),D2D1_ANTIALIAS_MODE_ALIASED);
    float so=g_scroll;
    for(auto& g: g_groups){
        float gy=g.headRc.top-so;
        // dot + glow
        float cx=padX+4*g_scale, cy=gy+7*g_scale, dr=4*g_scale;
        set(g.accent); D2D1_ELLIPSE gl=D2D1::Ellipse(D2D1::Point2F(cx,cy),dr*2,dr*2); D2D1_COLOR_F glow=g.accent; glow.a=0.35f; rt->FillEllipse(gl,set(glow));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx,cy),dr,dr),set(g.accent));
        rt->DrawText(g.name.c_str(),(UINT32)g.name.size(),g_fGroup,D2D1::RectF(padX+16*g_scale,gy-2*g_scale,W-padX-60*g_scale,gy+14*g_scale),set(P.text));
        wchar_t cnt[32]; swprintf(cnt,32,L"%d tasks",g.count);
        g_fSub->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        rt->DrawText(cnt,(UINT32)wcslen(cnt),g_fSub,D2D1::RectF(W-padX-100*g_scale,gy-2*g_scale,W-padX,gy+14*g_scale),set(P.subtle));
        g_fSub->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    for(size_t i=0;i<g_tiles.size();++i){
        auto& t=g_tiles[i]; bool sel=((int)i==g_sel);
        D2D1_RECT_F r=t.rc; r.top-=so; r.bottom-=so;
        if(r.bottom<g_areaTop||r.top>g_areaBot) continue;
        float trad=8*g_scale;
        if(sel){ D2D1_COLOR_F ring=ACC_FILL(); ring.a=0.28f; drawRound(rt,D2D1::RectF(r.left-3*g_scale,r.top-3*g_scale,r.right+3*g_scale,r.bottom+3*g_scale),trad+3*g_scale,set(ring));
                 drawRound(rt,r,trad,set(ACC_FILL()),set(ACC_BORDER()),1*g_scale); }
        else { drawRound(rt,r,trad,set(P.tile),set(P.tileBorder),1*g_scale); }
        // icon badge
        float bx=r.left+8*g_scale, by=r.top+(r.bottom-r.top-19*g_scale)/2, bs=19*g_scale;
        D2D1_RECT_F badge=D2D1::RectF(bx,by,bx+bs,by+bs);
        const IconPix* ip = iconPixFor(t.e.w.iconPng);
        ID2D1Bitmap* bmp = nullptr;
        if(ip){ D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                rt->CreateBitmap(D2D1::SizeU(ip->w,ip->h), ip->px.data(), ip->w*4, bp, &bmp); }
        if(bmp){ rt->DrawBitmap(bmp,badge,1.0f,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR); bmp->Release(); }
        else {
            D2D1_COLOR_F ac = machineAccentFor(i);
            drawRound(rt,badge,5*g_scale,set(ac));
            wchar_t ini[2]={t.initial,0};
            rt->DrawText(ini,1,g_fBadge,badge,set(D2D1::ColorF(1,1,1)));
        }
        // title
        D2D1_RECT_F tr=D2D1::RectF(bx+bs+8*g_scale, r.top, r.right-12*g_scale, r.bottom);
        g_fTile->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        IDWriteTextLayout* tl=nullptr; g_dw->CreateTextLayout(t.title.c_str(),(UINT32)t.title.size(),g_fTile,tr.right-tr.left,tr.bottom-tr.top,&tl);
        if(tl){ tl->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0}; IDWriteInlineObject* ell=nullptr; g_dw->CreateEllipsisTrimmingSign(g_fTile,&ell); tl->SetTrimming(&trim,ell);
            rt->DrawTextLayout(D2D1::Point2F(tr.left,tr.top),tl,set(sel?D2D1::ColorF(1,1,1):P.tileText)); if(ell)ell->Release(); tl->Release(); }
    }
    rt->PopAxisAlignedClip();

    // scrollbar — only when content actually overflows the (already content-fitted) task area
    if(g_contentH > g_areaBot + 0.5f){
        float viewH=g_areaBot-g_areaTop, scrollable=g_contentH-g_areaTop;
        float th=viewH*viewH/scrollable; float ms=maxScroll();
        float ty=g_areaTop + (ms>0 ? g_scroll/ms : 0)*(viewH-th);
        D2D1_COLOR_F sb=P.subtle; sb.a=0.35f; drawRound(rt,D2D1::RectF(W-6*g_scale,ty,W-2*g_scale,ty+th),3*g_scale,set(sb));
    }

    // footer
    float fy=g_areaBot; rt->DrawLine(D2D1::Point2F(padX,fy+18*g_scale),D2D1::Point2F(W-padX,fy+18*g_scale),set(P.line),1*g_scale);
    // key hints as kbd chips (matches the in-picker interaction, not the design's hold-Ctrl model)
    float fx=padX, cy=fy+26*g_scale, ch=16*g_scale;
    g_fKbd->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); g_fKbd->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_fSub->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    auto chip=[&](const wchar_t* t){ float tw=measureText(t,g_fKbd,120*g_scale); float w=tw+10*g_scale;
        D2D1_RECT_F r=D2D1::RectF(fx,cy,fx+w,cy+ch); drawRound(rt,r,4*g_scale,nullptr,set(P.kbdBorder),1*g_scale);
        rt->DrawText(t,(UINT32)wcslen(t),g_fKbd,r,set(P.kbdText)); fx+=w+4*g_scale; };
    auto lbl=[&](const wchar_t* t,float gap){ float tw=measureText(t,g_fSub,220*g_scale);
        rt->DrawText(t,(UINT32)wcslen(t),g_fSub,D2D1::RectF(fx,cy,fx+tw+4*g_scale,cy+ch),set(P.subtle)); fx+=tw+gap; };
    chip(L"\u2190\u2191\u2192\u2193"); lbl(L"task",14*g_scale);
    chip(L"Tab"); lbl(L"computer",14*g_scale);
    chip(L"\u21B5"); lbl(L"switch",14*g_scale);
    chip(L"Esc"); lbl(L"cancel",0);
    g_fKbd->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING); g_fKbd->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    g_fSub->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    if(br) br->Release();
    rt->EndDraw();
    GdiFlush();

    // acrylic behind (blur shows through our semi-transparent panel; graceful if unsupported)
    static PSWCA pSWCA=(PSWCA)GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetWindowCompositionAttribute");
    if(pSWCA){ ACCENT_POLICY ap{ACCENT_ENABLE_ACRYLICBLURBEHIND,0, useDark()?0x991A1B20u:0x99FFFFFFu,0}; WINCOMPATTRDATA d{19,&ap,sizeof ap}; pSWCA(g_win,&d); }

    // present via UpdateLayeredWindow, centered on the cursor monitor
    POINT ptSrc={0,0}; SIZE sz={W,H};
    int px=(mi.rcWork.left+mi.rcWork.right)/2 - W/2, py=(mi.rcWork.top+mi.rcWork.bottom)/2 - H/2;
    POINT ptPos={px,py}; BLENDFUNCTION bf{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    UpdateLayeredWindow(g_win,screen,&ptPos,&sz,memdc,&ptSrc,0,&bf,ULW_ALPHA);

    rt->Release();
    SelectObject(memdc,old); DeleteObject(dib); DeleteDC(memdc); ReleaseDC(nullptr,screen);
}
// per-tile fallback accent: find the group this flat index belongs to
static D2D1_COLOR_F machineAccentFor(size_t tileIdx){
    int acc=0; for(size_t g=0; g<g_groups.size(); ++g){ acc+=g_groups[g].count; if((int)tileIdx<acc) return g_groups[g].accent; }
    return machineAccent(0);
}

void showPicker(Hub& hub){
    g_hub=&hub;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // WIC needs COM on this thread
    WNDCLASSW wc{}; wc.lpfnWndProc=proc; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"TSWPicker";
    RegisterClassW(&wc);
    g_win=CreateWindowExW(WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_TOPMOST,L"TSWPicker",L"Unified Tasks",
                          WS_POPUP,0,0,560,440,nullptr,nullptr,wc.hInstance,nullptr);
    DWM_WINDOW_CORNER_PREFERENCE cp=DWMWCP_ROUND; DwmSetWindowAttribute(g_win,DWMWA_WINDOW_CORNER_PREFERENCE,&cp,sizeof cp);
    hub.setOnHotkey([](const std::string& ep,const std::string& fg){
        // Relayed hotkey from inside an RDP session: while open it means "cycle" (the user is
        // holding Ctrl and tapping ^ in the session); otherwise it opens the picker.
        if(g_visible){ g_ctrlTab=true; if(g_win) PostMessageW(g_win,WM_TSW_KEY,VK_RIGHT,0); return; }   // Ctrl+^ = next task (->)
        RemotePreselection* pre=new RemotePreselection{ep,fg};
        if(!g_win || !PostMessageW(g_win,WM_TSW_REMOTE_SHOW,0,(LPARAM)pre)) delete pre;
    });
    // Install the low-level keyboard hook on its OWN thread with nothing but a tight message pump.
    // A WH_KEYBOARD_LL callback is dispatched on the thread that installed it, and Windows silently
    // drops the hook if that callback isn't serviced within LowLevelHooksTimeout. Installing it on
    // the UI thread meant picker rendering/enumeration could starve it (the hook would die after a
    // while and the hotkey went dead until restart). On a dedicated thread the callback only reads a
    // couple of flags and PostMessages the UI thread, so it stays responsive indefinitely.
    std::thread([]{
        g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, kbProc, GetModuleHandleW(nullptr), 0);
        MSG hm; while(GetMessageW(&hm,nullptr,0,0)){ TranslateMessage(&hm); DispatchMessageW(&hm); }
        if(g_kbHook) UnhookWindowsHookEx(g_kbHook);
    }).detach();

    MSG msg; while(GetMessageW(&msg,nullptr,0,0)){ TranslateMessage(&msg); DispatchMessageW(&msg); }
}
}
