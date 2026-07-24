#include "pipe.hpp"
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")
namespace tsw {
bool PipeServer::start(const std::wstring& name){
    // The agent may run at HIGH integrity (elevated) while the mstsc plugin that connects runs at
    // MEDIUM integrity as the interactive user. A default pipe gets a high mandatory label and
    // rejects the lower-IL plugin (no-write-up). Least-privilege SD instead of Everyone/Low:
    //   D:(A;;GRGW;;;IU) - read+write for INTERACTIVE users only (the logged-on user's mstsc)
    //   (A;;GA;;;SY)     - full control for SYSTEM
    //   S:(ML;;NW;;;ME)  - Medium mandatory label (lets the medium-IL plugin write; blocks Low/AppContainer)
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof sa; sa.bInheritHandle = FALSE;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:(A;;GRGW;;;IU)(A;;GA;;;SY)S:(ML;;NW;;;ME)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr);
    h_ = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
        65536, 65536, 0, sa.lpSecurityDescriptor ? &sa : nullptr);
    if (sa.lpSecurityDescriptor) LocalFree(sa.lpSecurityDescriptor);
    if (h_ != INVALID_HANDLE_VALUE) { rev_ = CreateEventW(nullptr,TRUE,FALSE,nullptr); wev_ = CreateEventW(nullptr,TRUE,FALSE,nullptr); }
    return h_ != INVALID_HANDLE_VALUE;
}
// Overlapped/event-driven: a read blocks on its completion event (no polling); read and write use
// SEPARATE events so a blocked read never serializes with a concurrent write on the same handle.
bool PipeServer::waitForClient(){
    OVERLAPPED ov{}; ov.hEvent = rev_; ResetEvent(rev_);
    if (ConnectNamedPipe(h_, &ov)) return true;
    DWORD e = GetLastError();
    if (e == ERROR_PIPE_CONNECTED) return true;
    if (e == ERROR_IO_PENDING && WaitForSingleObject(rev_, INFINITE) == WAIT_OBJECT_0) { DWORD d; return GetOverlappedResult(h_,&ov,&d,FALSE) != 0; }
    return false;
}
int PipeServer::read(char* b,int n){
    OVERLAPPED ov{}; ov.hEvent = rev_; ResetEvent(rev_);
    DWORD got = 0;
    if (ReadFile(h_, b, n, &got, &ov)) return (int)got;
    if (GetLastError() == ERROR_IO_PENDING && WaitForSingleObject(rev_, INFINITE) == WAIT_OBJECT_0
        && GetOverlappedResult(h_, &ov, &got, FALSE)) return (int)got;
    return -1;
}
bool PipeServer::write(const char* b,int n){
    OVERLAPPED ov{}; ov.hEvent = wev_; ResetEvent(wev_);
    DWORD put = 0;
    if (WriteFile(h_, b, n, &put, &ov)) return (int)put == n;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    // bounded wait: a stuck write (full buffer / dead reader) must never hang the picker thread
    if (WaitForSingleObject(wev_, 2000) == WAIT_OBJECT_0 && GetOverlappedResult(h_, &ov, &put, FALSE)) return (int)put == n;
    CancelIoEx(h_, &ov); GetOverlappedResult(h_, &ov, &put, TRUE);   // reclaim the OVERLAPPED before returning
    return false;
}
void PipeServer::close(){
    if (h_ != INVALID_HANDLE_VALUE) { CancelIoEx(h_, nullptr); DisconnectNamedPipe(h_); CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
    if (rev_) { CloseHandle(rev_); rev_ = nullptr; }
    if (wev_) { CloseHandle(wev_); wev_ = nullptr; }
}
// Overlapped/event-driven, mirroring PipeServer: the worker thread blocks in an overlapped read
// (no busy-poll) while OnDataReceived writes on the same handle — separate completion events mean
// the two never serialize. cancelIo() lets the owning thread unblock a pending read before teardown.
bool PipeClient::connect(const std::wstring& name){
    h_ = CreateFileW(name.c_str(), GENERIC_READ|GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) return false;
    rev_ = CreateEventW(nullptr,TRUE,FALSE,nullptr); wev_ = CreateEventW(nullptr,TRUE,FALSE,nullptr);
    return true;
}
int PipeClient::read(char* b,int n){
    OVERLAPPED ov{}; ov.hEvent = rev_; ResetEvent(rev_);
    DWORD got = 0;
    if (ReadFile(h_, b, n, &got, &ov)) return (int)got;
    if (GetLastError() == ERROR_IO_PENDING && WaitForSingleObject(rev_, INFINITE) == WAIT_OBJECT_0
        && GetOverlappedResult(h_, &ov, &got, FALSE)) return (int)got;
    return -1;   // disconnect / error / cancelled
}
bool PipeClient::write(const char* b,int n){
    OVERLAPPED ov{}; ov.hEvent = wev_; ResetEvent(wev_);
    DWORD put = 0;
    if (WriteFile(h_, b, n, &put, &ov)) return (int)put == n;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(wev_, 2000) == WAIT_OBJECT_0 && GetOverlappedResult(h_, &ov, &put, FALSE)) return (int)put == n;
    CancelIoEx(h_, &ov); GetOverlappedResult(h_, &ov, &put, TRUE);   // reclaim the OVERLAPPED before returning
    return false;
}
void PipeClient::cancelIo(){ if (h_ != INVALID_HANDLE_VALUE) CancelIoEx(h_, nullptr); }
void PipeClient::close(){
    if (h_ != INVALID_HANDLE_VALUE) { CancelIoEx(h_, nullptr); CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
    if (rev_) { CloseHandle(rev_); rev_ = nullptr; }
    if (wev_) { CloseHandle(wev_); wev_ = nullptr; }
}
}
