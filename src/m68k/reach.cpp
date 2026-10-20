#include "m68k/reach.h"

#include <algorithm>
#include <set>

namespace m68k {

ReachResult reach(const std::vector<uint32_t> &seeds, const ReadWord &read) {
    ReachResult r;
    std::vector<uint32_t> work(seeds.rbegin(), seeds.rend());
    std::set<uint32_t> sites, stops, unmapped;
    while (!work.empty()) {
        uint32_t a = work.back();
        work.pop_back();
        while (!r.insns.count(a)) {
            if (a & 1) { unmapped.insert(a); break; }
            Insn in;
            if (!decode(a, read, in)) { unmapped.insert(a); break; }
            if (in.op == Op::Illegal || in.op == Op::LineA || in.op == Op::LineF) { stops.insert(a); }
            r.insns.emplace(a, in);
            if (is_branch(in)) work.push_back(in.target);
            if ((in.op == Op::Jmp || in.op == Op::Jsr) && !is_indirect(in))
                work.push_back(in.src.mode == Mode::PcDisp ? in.src.addr : in.src.addr);
            if (is_indirect(in)) sites.insert(a);
            if (ends_block(in)) break;
            a += in.length;
        }
    }
    r.indirect_sites.assign(sites.begin(), sites.end());
    r.stops.assign(stops.begin(), stops.end());
    r.unmapped.assign(unmapped.begin(), unmapped.end());
    return r;
}

std::vector<uint32_t> vector_seeds(const ReadWord &read) {
    std::vector<uint32_t> out;
    for (uint32_t v = 1; v < 64; ++v) {
        {
            const auto hi = read(v * 4), lo = read(v * 4 + 2);
            if (!hi || !lo) continue;
            const uint32_t t = (uint32_t(*hi) << 16) | *lo;
            if (t & 1 || !read(t)) continue;
            if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
        }
    }
    return out;
}

} // namespace m68k
