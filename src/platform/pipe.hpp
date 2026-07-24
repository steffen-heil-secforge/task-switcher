#pragma once
#include <string>
#include <windows.h>
namespace tsw {
inline const wchar_t* kPipeName = L"\\\\.\\pipe\\tsw_hub";
class PipeServer {
public:
    bool start(const std::wstring& name);
    bool waitForClient();
    int  read(char* buf, int n);      // BLOCKS (overlapped) until data; -1 on disconnect/error
    bool write(const char* buf, int n);   // overlapped, bounded wait so it never hangs the caller
    void close();
private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    HANDLE rev_ = nullptr, wev_ = nullptr;   // separate read/write completion events
};
class PipeClient {
public:
    bool connect(const std::wstring& name);
    int  read(char* buf, int n);      // BLOCKS (overlapped) until data; -1 on disconnect/error/cancel
    bool write(const char* buf, int n);   // overlapped, bounded wait so it never hangs the caller
    void cancelIo();                  // unblock a pending read (from another thread) before close/teardown
    void close();
private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    HANDLE rev_ = nullptr, wev_ = nullptr;   // separate read/write completion events
};
}
