#include "activation.hpp"
#include <cstdlib>
#include <cstdint>
namespace tsw {
bool shouldForegroundLocally(const std::string& e, const std::string& self){ return e == self; }
void* parseHwnd(const std::string& hex){ return (void*)(uintptr_t)strtoull(hex.c_str(), nullptr, 16); }
}
