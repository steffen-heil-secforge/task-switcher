#include <windows.h>
#include <tsvirtualchannels.h>   // IWTSPlugin, IWTSListenerCallback, IWTSVirtualChannelCallback
#include <unknwn.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <fstream>
#include <cstdlib>
#include <string>
#include <cstdio>
#include "pipe.hpp"
#include "protocol.hpp"
using namespace tsw;

static void L(const char* s, long n = -1){
    static const char* dir = std::getenv("TSW_LOG");   // opt-in diagnostics only
    if(!dir || !*dir) return;
    std::ofstream f(std::string(dir) + "\\plugin.log", std::ios::app);
    f << GetTickCount64() << "  " << s;
    if (n >= 0) f << " " << n;
    f << "\n";
}

// One channel connection: bridges DVC channel bytes <-> named pipe to the Client Agent.
class ChannelCallback : public IWTSVirtualChannelCallback {
    LONG ref_ = 1; IWTSVirtualChannel* chan_; PipeClient pipe_; bool connected_ = false; bool sentBridge_ = false;
    std::mutex mu_;                       // guards pipe_/connected_/sentBridge_ (OnDataReceived vs worker)
    std::thread worker_; std::atomic<bool> run_{false};
    // Dedicated thread: forward client->server bytes (e.g. ActivateRequest) to the channel as soon
    // as they arrive. Event-driven — it BLOCKS in an overlapped read (no busy-poll); the pipe's
    // separate read/write completion events mean this read never serializes with OnDataReceived's
    // write on the same handle. The worker touches only pipe_.read() + chan_->Write() and never the
    // mutex, so the owning (mstsc) thread can stopWorker() while holding mu_ without deadlocking.
    void startWorker(){
        if (run_.exchange(true)) return;
        if (worker_.joinable()) worker_.join();   // reap a worker that exited on a broken pipe
        worker_ = std::thread([this]{
            char b[8192];
            while (run_.load()) {
                int n = pipe_.read(b, sizeof b);   // blocks until data, disconnect, or cancelIo()
                if (!run_.load()) break;           // stopped during the read
                if (n > 0) chan_->Write((ULONG)n, (BYTE*)b, nullptr);   // Write off the mstsc thread
                else break;                        // pipe broke -> exit; OnDataReceived reconnects + restarts us
            }
        });
    }
    // Stop the worker: unblock its pending read via cancelIo, then join. Safe to call under mu_ —
    // the worker never takes mu_. Resets run_ so a subsequent startWorker() actually restarts it.
    void stopWorker(){
        run_ = false;
        pipe_.cancelIo();
        if (worker_.joinable()) worker_.join();
    }
public:
    // Minimal ctor: no blocking work here (runs inside OnNewChannelConnection). Pipe connects
    // lazily on first data; the worker thread starts once connected.
    ChannelCallback(IWTSVirtualChannel* c) : chan_(c) { L("ChannelCallback created"); }
    ~ChannelCallback(){ stopWorker(); }
    HRESULT __stdcall QueryInterface(REFIID riid, void** o){
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IWTSVirtualChannelCallback)) {
            *o = static_cast<IWTSVirtualChannelCallback*>(this); AddRef(); return S_OK;
        }
        *o = nullptr; return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef(){ return InterlockedIncrement(&ref_); }
    ULONG __stdcall Release(){ LONG r=InterlockedDecrement(&ref_); if(!r){ delete this; } return r; }
    HRESULT __stdcall OnDataReceived(ULONG cb, BYTE* data){
        {
            std::lock_guard<std::mutex> g(mu_);
            if (!connected_) {
                stopWorker();   // reap any worker still bound to a stale handle before reconnecting
                pipe_.close();
                for (int i = 0; i < 25 && !connected_; i++) { connected_ = pipe_.connect(kPipeName); if (!connected_) Sleep(200); }
                sentBridge_ = false;
                L("lazy pipe connect ok=", connected_ ? 1 : 0);
            }
            // Report our host mstsc PID once so the client maps this connection to the right mstsc window.
            if (connected_ && !sentBridge_) {
                sentBridge_ = true;
                Message b; b.type = MsgType::Bridge; b.requestId = (int)GetCurrentProcessId();
                std::string bf = encodeFrame(b);
                pipe_.write(bf.data(), (int)bf.size());
                L("sent Bridge PID=", (long)b.requestId);
            }
            bool w = false;
            if (connected_ && data && cb > 0 && cb <= (1u << 20)) w = pipe_.write((char*)data, (int)cb);   // server->client
            // Pipe broken (client agent restarted)? Drop it so we reconnect + re-Bridge next time.
            if (connected_ && data && cb > 0 && !w) { stopWorker(); pipe_.close(); connected_ = false; sentBridge_ = false; L("pipe write failed -> will reconnect"); }
        }
        if (connected_) startWorker();   // client->server is handled by the worker thread now
        return S_OK;
    }
    HRESULT __stdcall OnClose(){ L("OnClose"); stopWorker(); std::lock_guard<std::mutex> g(mu_); pipe_.close(); return S_OK; }
};

class ListenerCallback : public IWTSListenerCallback {
    LONG ref_=1;
public:
    HRESULT __stdcall QueryInterface(REFIID riid,void**o){
        if(riid==__uuidof(IUnknown)||riid==__uuidof(IWTSListenerCallback)){*o=static_cast<IWTSListenerCallback*>(this);AddRef();return S_OK;}
        *o=nullptr;return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef(){return InterlockedIncrement(&ref_);}
    ULONG __stdcall Release(){LONG r=InterlockedDecrement(&ref_);if(!r)delete this;return r;}
    HRESULT __stdcall OnNewChannelConnection(IWTSVirtualChannel* ch, BSTR, BOOL* pAccept,
                                             IWTSVirtualChannelCallback** ppCb){
        L("OnNewChannelConnection");
        *pAccept=TRUE; *ppCb=new ChannelCallback(ch); return S_OK;
    }
};

class Plugin : public IWTSPlugin {
    LONG ref_=1;
public:
    HRESULT __stdcall QueryInterface(REFIID riid,void**o){
        if(riid==__uuidof(IUnknown)||riid==__uuidof(IWTSPlugin)){*o=static_cast<IWTSPlugin*>(this);AddRef();return S_OK;}
        *o=nullptr;return E_NOINTERFACE;
    }
    ULONG __stdcall AddRef(){return InterlockedIncrement(&ref_);}
    ULONG __stdcall Release(){LONG r=InterlockedDecrement(&ref_);if(!r)delete this;return r;}
    HRESULT __stdcall Initialize(IWTSVirtualChannelManager* mgr){
        HRESULT hr = mgr->CreateListener("TSWLIST", 0, new ListenerCallback(), nullptr);
        L("Initialize: CreateListener hr=", (long)hr);
        return hr;
    }
    HRESULT __stdcall Connected(){ L("Connected"); return S_OK; }
    HRESULT __stdcall Disconnected(DWORD){ return S_OK; }
    HRESULT __stdcall Terminated(){ return S_OK; }
};

extern "C" __declspec(dllexport) HRESULT VirtualChannelGetInstance(REFIID, ULONG* pNum, void** ppInst){
    if (!ppInst){ *pNum=1; return S_OK; }
    *pNum=1; ppInst[0]=static_cast<IWTSPlugin*>(new Plugin()); return S_OK;
}
