// Shuffle as a bag: every track of the list is chosen once before any is chosen twice. (A plain random pick repeats a track in a list of 1000
// after about 40 songs.) The round starts again when every track has been chosen; the track that plays now is never the next one.
#pragma once
#include <random>
#include <set>
#include <string>
#include <vector>

namespace muisc {

class ShuffleBag {
public:
    // keys: one string per entry of the list that is shown (the path of a file, the id of an online track); base: the entry that plays now
    // (-1 = none). Returns the index of the next one.
    int pick(const std::vector<std::string>& keys, int base, std::mt19937& rng) {
        const int len = static_cast<int>(keys.size());
        if (len <= 1) return 0;
        auto candidates = [&]() {
            std::vector<int> c;
            for (int i = 0; i < len; ++i) if (i != base && heard_.count(keys[static_cast<size_t>(i)]) == 0) c.push_back(i);
            return c;
        };
        std::vector<int> c = candidates();
        if (c.empty()) { heard_.clear(); c = candidates(); }   // the round is over
        if (base >= 0 && base < len) heard_.insert(keys[static_cast<size_t>(base)]);   // what plays now counts as heard
        const int next = c[std::uniform_int_distribution<size_t>(0, c.size() - 1)(rng)];
        heard_.insert(keys[static_cast<size_t>(next)]);
        return next;
    }
    void clear() { heard_.clear(); }
    size_t heard() const { return heard_.size(); }

private:
    std::set<std::string> heard_;
};

} // namespace muisc
