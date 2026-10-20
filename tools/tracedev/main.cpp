// tracedev: what the i960 did with one device range in a trace: for each
// address, lane mask and direction, how often, from which epoch, and the
// values seen (most frequent first). For working out a device's protocol
// before modelling it natively.
//
//   tracedev TRACE.m2tr START END [MAXVALUES]

#include "trace/trace.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <tuple>
#include <vector>

int main(int argc, char **argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: tracedev TRACE.m2tr START END [MAXVALUES]\n");
        return 2;
    }
    const uint32_t lo = uint32_t(std::strtoul(argv[2], nullptr, 0)), hi = uint32_t(std::strtoul(argv[3], nullptr, 0));
    const size_t maxv = argc > 4 ? size_t(std::atoi(argv[4])) : 6;
    trace::Reader r(argv[1]);
    if (!r.error().empty()) {
        std::fprintf(stderr, "tracedev: %s\n", r.error().c_str());
        return 2;
    }
    struct Stat {
        uint64_t n = 0, first = 0, last = 0;
        std::map<uint32_t, uint64_t> values;
    };
    std::map<std::tuple<uint32_t, uint32_t, bool>, Stat> stats;
    trace::Epoch e;
    uint64_t epoch = 0;
    while (r.next(e)) {
        for (const auto &a : e.events) {
            if (a.stalled || a.addr < lo || a.addr > hi) continue;
            Stat &s = stats[{a.addr, a.mask, a.write}];
            if (!s.n++) s.first = epoch;
            s.last = epoch;
            if (s.values.size() < 4096) s.values[a.data & a.mask]++;
        }
        ++epoch;
    }
    for (const auto &[k, s] : stats) {
        const auto &[addr, mask, write] = k;
        std::vector<std::pair<uint64_t, uint32_t>> v;
        for (auto [val, n] : s.values) v.push_back({n, val});
        std::sort(v.rbegin(), v.rend());
        std::printf("%s %08x mask %08x  n %-8llu epochs %llu-%llu  %zu value(s):", write ? "W" : "R", addr, mask,
                    (unsigned long long)s.n, (unsigned long long)s.first, (unsigned long long)s.last, s.values.size());
        for (size_t i = 0; i < v.size() && i < maxv; i++) std::printf(" %x(%llu)", v[i].second, (unsigned long long)v[i].first);
        std::printf("\n");
    }
}
