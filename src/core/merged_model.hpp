#pragma once
#include <string>
#include <vector>
#include <utility>
#include "protocol.hpp"
namespace tsw {
struct Entry { std::string endpointId; WindowInfo w; };
class MergedModel {
public:
    void applySnapshot(const std::string& ep, const std::vector<WindowInfo>& ws);
    void applyDelta(const std::string& ep, const std::vector<WindowInfo>& added,
                    const std::vector<std::string>& removedHwnds, const std::vector<WindowInfo>& updated);
    void dropEndpoint(const std::string& ep);
    std::vector<Entry> all() const;
private:
    std::vector<std::string> order_;               // endpoint insertion order
    std::vector<std::pair<std::string, std::vector<WindowInfo>>> data_;
    std::vector<WindowInfo>* find(const std::string& ep);
    std::vector<WindowInfo>& ensure(const std::string& ep);
};
}
