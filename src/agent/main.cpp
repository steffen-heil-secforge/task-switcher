// Unified agent entry point. One binary serves both roles; which one it plays is decided at
// runtime by whether this process is running inside an RDP session:
//   - NOT a remote session  -> the user's own PC          -> client agent (picker/hub/hotkey)
//   - IS a remote session   -> a server the user is on    -> session agent (DVC channel + hook)
// The mstsc DVC plugin (tsw_plugin.dll) stays a separate DLL because mstsc LoadLibrary's it.
#include <windows.h>

int runClientAgent();    // src/client_agent/client_agent.cpp
int runSessionAgent();   // src/session_agent/session_agent.cpp

int main(){
    return GetSystemMetrics(SM_REMOTESESSION) ? runSessionAgent() : runClientAgent();
}
