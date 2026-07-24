#include "merged_model.hpp"
#include <algorithm>
namespace tsw {
std::vector<WindowInfo>* MergedModel::find(const std::string& ep){
    for (auto& kv : data_) if (kv.first==ep) return &kv.second; return nullptr;
}
std::vector<WindowInfo>& MergedModel::ensure(const std::string& ep){
    if (auto* p=find(ep)) return *p;
    order_.push_back(ep); data_.push_back({ep,{}}); return data_.back().second;
}
void MergedModel::applySnapshot(const std::string& ep, const std::vector<WindowInfo>& ws){
    // Stable merge (not a wholesale replace): keep existing entries and their order, update the
    // matched ones in place, append genuinely-new ones at the end, and drop only those absent from
    // the new snapshot. This avoids reordering rows / disrupting the picker's selection & preselect
    // as fresh snapshots arrive.
    auto& cur = ensure(ep);
    std::vector<bool> placed(ws.size(), false);
    for (auto& c : cur)
        for (size_t i = 0; i < ws.size(); ++i)
            if (!placed[i] && ws[i].hwnd == c.hwnd) { c = ws[i]; placed[i] = true; break; }
    cur.erase(std::remove_if(cur.begin(), cur.end(), [&](const WindowInfo& c){
        for (auto& w : ws) if (w.hwnd == c.hwnd) return false; return true; }), cur.end());
    for (size_t i = 0; i < ws.size(); ++i) if (!placed[i]) cur.push_back(ws[i]);
}
void MergedModel::applyDelta(const std::string& ep, const std::vector<WindowInfo>& added,
                             const std::vector<std::string>& removed, const std::vector<WindowInfo>& updated){
    auto& v = ensure(ep);
    for (auto& r : removed) v.erase(std::remove_if(v.begin(),v.end(),
        [&](const WindowInfo& w){return w.hwnd==r;}), v.end());
    for (auto& u : updated) for (auto& w : v) if (w.hwnd==u.hwnd) w=u;
    for (auto& a : added) v.push_back(a);
}
void MergedModel::dropEndpoint(const std::string& ep){
    order_.erase(std::remove(order_.begin(),order_.end(),ep), order_.end());
    data_.erase(std::remove_if(data_.begin(),data_.end(),
        [&](std::pair<std::string, std::vector<WindowInfo>>& kv){return kv.first==ep;}), data_.end());
}
std::vector<Entry> MergedModel::all() const {
    std::vector<Entry> out;
    for (auto& ep : order_) for (auto& kv : data_) if (kv.first==ep)
        for (auto& w : kv.second) out.push_back({ep, w});
    return out;
}
}
