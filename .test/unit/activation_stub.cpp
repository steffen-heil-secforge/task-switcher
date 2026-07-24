// Test-only stubs: the Win32 activation calls live in activation_win.cpp (agent targets).
// Tests link hub.cpp (which references them) but never actually foreground a window.
namespace tsw { void foregroundWindow(void*) {} void showDesktop() {} }
