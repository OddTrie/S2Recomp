// The VU1 program generator: program signatures, the static analysis behind VU1_COMPILED_PAIR_S and
// the writer of vu1gen/, for ps2EntryRunner --vu-gen (code images from a recording, ps2_vu1_compiled.cpp)
// and ps2xVuGen (code read from the game's own files at the places a locations file names). Needs
// nothing of the runtime but the VU headers, so ps2xVuGen builds before the game. See ps2_vu1_compiled.h.
#include "ps2_vu1_compiled.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <utility>

namespace
{
    constexpr uint32_t kMaxPairs = VU1Core::kMaxDecodedPairs;

    void readPair(const uint8_t *code, uint32_t index, uint32_t &lower, uint32_t &upper)
    {
        std::memcpy(&lower, code + index * 8u, sizeof(lower));
        std::memcpy(&upper, code + index * 8u + 4u, sizeof(upper));
    }

    uint64_t fnv(uint64_t h, uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
        {
            h ^= (value >> (8 * i)) & 0xFFu;
            h *= 1099511628211ull;
        }
        return h;
    }
}

uint64_t vu1ProgramSignature(const uint8_t *code, uint32_t codeSize, uint32_t startPc, std::vector<uint32_t> *reachable)
{
    const uint32_t pairs = std::min<uint32_t>(codeSize / 8u, kMaxPairs);
    if (pairs == 0u || (startPc & 7u) != 0u)
        return 0u;
    // Same walk as programReadsFlags (ps2_vu1_core.cpp), over the raw words.
    std::vector<uint8_t> seen(pairs, 0u); // 1 = visited as a delay slot only, 2 = with fall-through
    std::vector<std::pair<uint32_t, bool>> work;
    std::vector<uint32_t> returnPairs;
    bool sawJump = false;
    auto push = [&](uint32_t index, bool noFallthrough) { work.push_back({index % pairs, noFallthrough}); };
    push(startPc / 8u, false);
    for (;;)
    {
        while (!work.empty())
        {
            const auto [index, noFallthrough] = work.back();
            work.pop_back();
            if (seen[index] == 2u || (seen[index] == 1u && noFallthrough))
                continue;
            seen[index] = noFallthrough ? 1u : 2u;
            uint32_t lower = 0, upper = 0;
            readPair(code, index, lower, upper);
            bool fallthrough = !noFallthrough;
            if ((upper & 0x80000000u) == 0u) // no I bit: the lower word is an instruction
            {
                int32_t imm = static_cast<int32_t>(lower & 0x7FFu);
                if (imm & 0x400)
                    imm -= 0x800;
                const uint32_t target = static_cast<uint32_t>(static_cast<int32_t>(index + 1u) + imm + static_cast<int32_t>(pairs)) % pairs;
                switch (lower >> 25)
                {
                case 0x20: // B
                case 0x21: // BAL
                    if ((lower >> 25) == 0x21u)
                        returnPairs.push_back(index + 2u);
                    push(index + 1u, true);
                    push(target, false);
                    fallthrough = false;
                    break;
                case 0x28: case 0x29: case 0x2C: case 0x2D: case 0x2E: case 0x2F: // IBxx
                    push(target, false);
                    break;
                case 0x24: // JR
                case 0x25: // JALR
                    if ((lower >> 25) == 0x25u)
                        returnPairs.push_back(index + 2u);
                    sawJump = true;
                    push(index + 1u, true);
                    fallthrough = false;
                    break;
                default:
                    break;
                }
            }
            if ((upper & 0x40000000u) != 0u) // E bit: the delay slot runs, then the program ends
            {
                push(index + 1u, true);
                fallthrough = false;
            }
            if (fallthrough)
                push(index + 1u, false);
        }
        if (!sawJump)
            break;
        bool added = false;
        for (uint32_t r : returnPairs)
            if (seen[r % pairs] != 2u)
            {
                push(r, false);
                added = true;
            }
        if (!added)
            break;
    }

    uint64_t h = fnv(1469598103934665603ull, startPc);
    if (reachable)
        reachable->clear();
    for (uint32_t index = 0; index < pairs; ++index)
    {
        if (seen[index] == 0u)
            continue;
        uint32_t lower = 0, upper = 0;
        readPair(code, index, lower, upper);
        h = fnv(fnv(fnv(h, index), lower), upper);
        if (reachable)
            reachable->push_back(index);
    }
    return h != 0u ? h : 1u;
}

namespace
{
    // Static analysis behind VU1_COMPILED_PAIR_S (the kElide* facts, see ps2_vu1_core.h). P = the
    // program's compiled pairs; only their words are known (other code in the image can differ
    // between images that match the signature). Edges: pair i -> i + 1, and the delay slot of a
    // branch -> its target (a superset of the real flow, also for a branch in a delay slot); JR/JALR
    // targets and anything leaving P are unknown. Predecessors of k: k - 1 (unknown if outside P)
    // and the delay slots in P of branches in P that target k; branches from outside P are caught
    // at run time (checkCompiledEntry), JR/JALR targets by the gate the jump sets.
    struct PairElision
    {
        uint32_t elide = 0, vfKeep = 0xFFFFu, viKeep = 0xFFu, accKeep = 0xFu;
    };

    struct ElisionStats
    {
        uint64_t pairs = 0, upperMark = 0, lowerMark = 0, viMark = 0, accMark = 0, ready = 0, readsKept = 0, readsAll = 0,
                 regUntil = 0, branch = 0, prevEnd = 0;
    };

    std::vector<PairElision> analyseElisions(const std::vector<uint8_t> &code, const std::vector<std::array<uint32_t, 3>> &compiled, ElisionStats &stats)
    {
        using D = VU1Core::DecodedInstructionPair;
        constexpr uint32_t kL = VU1Core::kMaxRegLatency;
        const uint32_t n = static_cast<uint32_t>(code.size() / 8u);
        std::vector<PairElision> result(compiled.size());
        if (n != kMaxPairs) // branch targets wrap at the VU1 pc mask: the analysis assumes 16 KB code
            return result;
        std::vector<uint8_t> inP(n, 0u);
        std::vector<D> dec(n);
        for (const auto &c : compiled)
        {
            inP[c[0]] = 1u;
            dec[c[0]] = VU1Core::decodePairWords(c[1], c[2], VU1Core::Unit::VU1);
        }
        auto prev = [n](uint32_t i) { return (i + n - 1u) % n; };
        auto isBranch = [&](uint32_t i) { return !dec[i].iBit && dec[i].lowerUsage.pipeline == VU1Core::PipelineBranch; };
        auto isJump = [&](uint32_t i) { return isBranch(i) && (dec[i].lower >> 26) == 0x12u; };
        auto target = [&](uint32_t i)
        {
            int32_t imm = static_cast<int32_t>(dec[i].lower & 0x7FFu);
            if (imm & 0x400)
                imm -= 0x800;
            return static_cast<uint32_t>(static_cast<int32_t>(i + 1u) + imm + static_cast<int32_t>(n)) % n;
        };
        auto vfLat = [](const VU1Core::InstructionUsage &u) { return static_cast<uint32_t>(u.vfLatency != 0u ? u.vfLatency : u.latency); };
        auto viLat = [](const VU1Core::InstructionUsage &u) { return static_cast<uint32_t>(u.viLatency != 0u ? u.viLatency : u.latency); };
        auto hasLowerVf = [&](uint32_t i) { return dec[i].lowerUsage.vfWrite.reg != 0u && dec[i].suppressedLowerVf != dec[i].lowerUsage.vfWrite.reg; };
        auto viWrites = [&](uint32_t i) { return static_cast<uint32_t>(dec[i].lowerUsage.viWrite) & 0xFFFEu; };
        // Latency of pair p's write to VF component rc (reg * 4 + c) / to VI reg (0 = no write).
        auto vfWriteLat = [&](uint32_t p, uint32_t rc)
        {
            uint32_t lat = 0;
            const uint32_t reg = rc / 4u, lane = laneForComponent(rc % 4u);
            const auto &u = dec[p].upperUsage;
            if (u.vfWrite.reg != 0u && u.vfWrite.reg == reg && (u.vfWrite.lanes & lane) != 0u)
                lat = std::max(lat, vfLat(u));
            const auto &l = dec[p].lowerUsage;
            if (hasLowerVf(p) && l.vfWrite.reg == reg && (l.vfWrite.lanes & lane) != 0u)
                lat = std::max(lat, vfLat(l));
            return lat;
        };
        auto viWriteLat = [&](uint32_t p, uint32_t reg) { return ((viWrites(p) >> reg) & 1u) != 0u ? viLat(dec[p].lowerUsage) : 0u; };
        auto readsVf = [&](uint32_t s, uint32_t rc)
        {
            for (uint32_t k = 0; k < dec[s].readyVfCount; ++k)
                if (dec[s].readyVf[k] == rc)
                    return true;
            return false;
        };
        auto readsVi = [&](uint32_t s, uint32_t reg)
        {
            for (uint32_t k = 0; k < dec[s].readyViCount; ++k)
                if (dec[s].readyVi[k] == reg)
                    return true;
            return false;
        };
        // Every write must fit the gate (kMaxRegLatency); otherwise no elisions in this program.
        for (const auto &c : compiled)
        {
            const uint32_t i = c[0];
            if ((dec[i].upperUsage.vfWrite.reg != 0u && vfLat(dec[i].upperUsage) > kL) || (hasLowerVf(i) && vfLat(dec[i].lowerUsage) > kL) ||
                (viWrites(i) != 0u && viLat(dec[i].lowerUsage) > kL))
            {
                std::printf("[vugen]   no elisions: pair %u (lower 0x%08x upper 0x%08x) writes with latency > %u (upper vf %u, lower vf %u, vi %u)\n",
                            i, dec[i].lower, dec[i].upper, kL, dec[i].upperUsage.vfWrite.reg != 0u ? vfLat(dec[i].upperUsage) : 0u,
                            hasLowerVf(i) ? vfLat(dec[i].lowerUsage) : 0u, viWrites(i) != 0u ? viLat(dec[i].lowerUsage) : 0u);
                return result;
            }
        }
        // Successors of i (false = unknown).
        auto succ = [&](uint32_t i, uint32_t out[2], uint32_t &count)
        {
            count = 0;
            const uint32_t p = prev(i);
            if (!inP[p])
                return false; // cannot tell whether i is a delay slot
            out[count++] = (i + 1u) % n;
            if (isBranch(p))
            {
                if (isJump(p))
                    return false;
                out[count++] = target(p);
            }
            for (uint32_t k = 0; k < count; ++k)
                if (!inP[out[k]])
                    return false;
            return true;
        };
        // Predecessors in P; open(k) = k - 1 is unknown code.
        std::vector<std::vector<uint32_t>> preds(n);
        for (const auto &c : compiled)
        {
            const uint32_t y = c[0];
            if (inP[prev(y)])
                preds[y].push_back(prev(y));
        }
        for (const auto &c : compiled)
        {
            const uint32_t y = c[0], b = prev(y);
            if (inP[b] && isBranch(b) && !isJump(b) && inP[target(b)] && target(b) != (y + 1u) % n)
                preds[target(b)].push_back(y);
        }
        auto open = [&](uint32_t k) { return !inP[prev(k)]; };
        // Some path from i reaches, within maxDepth pairs, a pair for which reads() holds (or unknown code).
        auto readerAhead = [&](uint32_t i, uint32_t maxDepth, auto &&reads)
        {
            std::vector<uint32_t> frontier{i}, next;
            for (uint32_t d = 1; d <= maxDepth; ++d)
            {
                next.clear();
                for (uint32_t x : frontier)
                {
                    uint32_t out[2], count = 0;
                    if (!succ(x, out, count))
                        return true;
                    for (uint32_t k = 0; k < count; ++k)
                    {
                        if (reads(out[k]))
                            return true;
                        next.push_back(out[k]);
                    }
                }
                std::sort(next.begin(), next.end());
                next.erase(std::unique(next.begin(), next.end()), next.end());
                frontier.swap(next);
            }
            return false;
        };
        // Some path into k has, d pairs back, a write with latency > d (d < kMaxRegLatency), or unknown code.
        auto writerBehind = [&](uint32_t k, auto &&writeLat)
        {
            std::vector<uint32_t> frontier{k}, next;
            for (uint32_t d = 1; d < kL; ++d)
            {
                next.clear();
                for (uint32_t x : frontier)
                {
                    if (open(x))
                        return true;
                    for (uint32_t p : preds[x])
                    {
                        if (writeLat(p) > d)
                            return true;
                        next.push_back(p);
                    }
                }
                std::sort(next.begin(), next.end());
                next.erase(std::unique(next.begin(), next.end()), next.end());
                frontier.swap(next);
            }
            return false;
        };
        // p can make the program end after the pair that follows it (E bit, D/T-bit branch).
        auto endsBefore = [&](uint32_t p) { return dec[p].eBit || (isBranch(p) && (dec[p].dBit || dec[p].tBit)); };
        // s may be the last pair executed when it follows `from`.
        auto mayEndAfter = [&](uint32_t s, uint32_t from) { return dec[s].dBit || dec[s].tBit || endsBefore(from); };
        auto pairWriteLat = [&](uint32_t s)
        {
            uint32_t lat = 0;
            if (dec[s].upperUsage.vfWrite.reg != 0u)
                lat = std::max(lat, vfLat(dec[s].upperUsage));
            if (hasLowerVf(s))
                lat = std::max(lat, vfLat(dec[s].lowerUsage));
            if (dec[s].upperUsage.accWrite != 0u)
                lat = std::max(lat, VU1Core::kAccForwardLatency);
            if (viWrites(s) != 0u)
                lat = std::max(lat, viLat(dec[s].lowerUsage));
            return lat;
        };
        // Every path from s (d pairs after the write, reached from `from`) meets a later write that
        // raises regWritesUntil at least as far, before the program can end.
        auto dominated = [&](auto &&self, uint32_t s, uint32_t from, uint32_t d, uint32_t lat) -> bool
        {
            if (d > 0u && pairWriteLat(s) != 0u && d + pairWriteLat(s) >= lat)
                return true;
            if (d >= kL || (d > 0u && mayEndAfter(s, from)))
                return false;
            uint32_t out[2], count = 0;
            if (!succ(s, out, count))
                return false;
            for (uint32_t k = 0; k < count; ++k)
                if (!self(self, out[k], s, d + 1u, lat))
                    return false;
            return true;
        };
        auto readsLanes = [&](uint32_t reg, uint32_t lanes)
        {
            return [&, reg, lanes](uint32_t s)
            {
                for (uint32_t c = 0; c < 4u; ++c)
                    if ((lanes & laneForComponent(c)) != 0u && readsVf(s, reg * 4u + c))
                        return true;
                return false;
            };
        };

        for (size_t ci = 0; ci < compiled.size(); ++ci)
        {
            const uint32_t i = compiled[ci][0];
            const D &x = dec[i];
            PairElision &e = result[ci];
            ++stats.pairs;
            // Forward: ready-table entries nobody reads inside the latency window.
            if (x.upperUsage.vfWrite.reg != 0u &&
                !readerAhead(i, vfLat(x.upperUsage) - 1u, readsLanes(x.upperUsage.vfWrite.reg, x.upperUsage.vfWrite.lanes)))
            {
                e.elide |= VU1Core::kElideUpperMark;
                ++stats.upperMark;
            }
            if (hasLowerVf(i) && !readerAhead(i, vfLat(x.lowerUsage) - 1u, readsLanes(x.lowerUsage.vfWrite.reg, x.lowerUsage.vfWrite.lanes)))
            {
                e.elide |= VU1Core::kElideLowerMark;
                ++stats.lowerMark;
            }
            if (const uint32_t vi = viWrites(i))
            {
                if (!readerAhead(i, viLat(x.lowerUsage) - 1u, [&](uint32_t s) {
                        for (uint32_t r = 1; r < 16u; ++r)
                            if (((vi >> r) & 1u) != 0u && readsVi(s, r))
                                return true;
                        return false; }))
                {
                    e.elide |= VU1Core::kElideViMark;
                    ++stats.viMark;
                }
            }
            if (x.upperUsage.accWrite != 0u)
            {
                e.elide |= VU1Core::kElideAccMark; // latency 1: never read inside its window
                ++stats.accMark;
            }
            // Backward: reads for which no write inside its latency window can still be pending.
            uint32_t vfKeep = 0, viKeep = 0;
            const uint32_t accKeep = 0; // ACC writes have latency 1: never pending at a later pair
            for (uint32_t k = 0; k < x.readyVfCount; ++k)
            {
                const uint32_t rc = x.readyVf[k];
                if (writerBehind(i, [&](uint32_t p) { return vfWriteLat(p, rc); }))
                    vfKeep |= 1u << k;
            }
            for (uint32_t k = 0; k < x.readyViCount; ++k)
            {
                const uint32_t reg = x.readyVi[k];
                if (writerBehind(i, [&](uint32_t p) { return viWriteLat(p, reg); }))
                    viKeep |= 1u << k;
            }
            const uint32_t allVf = (1u << x.readyVfCount) - 1u, allVi = (1u << x.readyViCount) - 1u;
            stats.readsAll += x.readyVfCount + x.readyViCount + std::popcount(static_cast<uint32_t>(x.readyAccComponents));
            stats.readsKept += std::popcount(vfKeep) + std::popcount(viKeep);
            if (vfKeep != allVf || viKeep != allVi || x.readyAccComponents != 0u)
            {
                e.elide |= VU1Core::kElideReady;
                e.vfKeep = vfKeep;
                e.viKeep = viKeep;
                e.accKeep = accKeep;
                ++stats.ready;
            }
            // Backward control state: what can have run just before i.
            const bool predsKnown = !open(i);
            bool predBranch = false, predEnd = false;
            for (uint32_t p : preds[i])
            {
                predBranch |= isBranch(p);
                predEnd |= endsBefore(p);
            }
            if (predsKnown && !predBranch && !isBranch(i))
            {
                e.elide |= VU1Core::kElideBranch;
                ++stats.branch;
            }
            if (predsKnown && !predEnd)
            {
                e.elide |= VU1Core::kElidePrevEnd;
                ++stats.prevEnd;
            }
            // regWritesUntil: a later write on every path raises it as far, before the program can end.
            if (const uint32_t lat = pairWriteLat(i);
                lat != 0u && predsKnown && !predEnd && !x.dBit && !x.tBit && dominated(dominated, i, i, 0u, lat))
            {
                e.elide |= VU1Core::kElideRegUntil;
                ++stats.regUntil;
            }
        }
        return result;
    }

    // Lanes known normalized at the entry of each pair (by pair index), over every path of the
    // program's static flow: nothing at the start, at open pairs (unknown code before them), after
    // a BAL/JALR's delay slot (the JR back clears m_normTrust) and after an E bit's; elsewhere the
    // intersection over the predecessors (as analyseElisions builds them) of the lanes after them.
    // Branches from outside the program and JR/JALR targets clear m_normTrust at run time.
    std::vector<vu1detail::NormLanes> analyseNormEntry(const std::vector<uint8_t> &code, const std::vector<std::array<uint32_t, 3>> &compiled,
                                                       uint32_t startPc)
    {
        using vu1detail::NormLanes;
        const uint32_t n = static_cast<uint32_t>(code.size() / 8u);
        std::vector<NormLanes> known(n);
        if (n != kMaxPairs)
            return known;
        std::vector<uint8_t> inP(n, 0u), entry(n, 0u);
        std::vector<VU1Core::DecodedInstructionPair> dec(n);
        for (const auto &c : compiled)
        {
            inP[c[0]] = 1u;
            dec[c[0]] = VU1Core::decodePairWords(c[1], c[2], VU1Core::Unit::VU1);
        }
        auto prev = [n](uint32_t i) { return (i + n - 1u) % n; };
        auto isBranch = [&](uint32_t i) { return !dec[i].iBit && dec[i].lowerUsage.pipeline == VU1Core::PipelineBranch; };
        auto isJump = [&](uint32_t i) { return isBranch(i) && (dec[i].lower >> 26) == 0x12u; };
        auto target = [&](uint32_t i)
        {
            int32_t imm = static_cast<int32_t>(dec[i].lower & 0x7FFu);
            if (imm & 0x400)
                imm -= 0x800;
            return static_cast<uint32_t>(static_cast<int32_t>(i + 1u) + imm + static_cast<int32_t>(n)) % n;
        };
        std::vector<std::vector<uint32_t>> preds(n);
        for (const auto &c : compiled)
        {
            const uint32_t y = c[0], b = prev(y);
            if (inP[b])
                preds[y].push_back(b);
            else
                entry[y] = 1u;
            if (inP[b] && isBranch(b) && !isJump(b) && inP[target(b)] && target(b) != (y + 1u) % n)
                preds[target(b)].push_back(y);
            const bool link = isBranch(c[0]) && (static_cast<uint32_t>(dec[c[0]].lowerUsage.viWrite) & 0xFFFEu) != 0u;
            if (link || dec[c[0]].eBit)
                entry[(c[0] + 2u) % n] = 1u;
        }
        entry[(startPc / 8u) % n] = 1u;
        constexpr NormLanes kAll{~0ull, ~0ull};
        for (const auto &c : compiled)
            known[c[0]] = entry[c[0]] ? NormLanes{} : kAll;
        for (bool changed = true; changed;)
        {
            changed = false;
            for (const auto &c : compiled)
            {
                const uint32_t y = c[0];
                if (entry[y])
                    continue;
                NormLanes k = kAll;
                for (uint32_t p : preds[y])
                {
                    const NormLanes after = vu1detail::normAfterPair(known[p], dec[p]);
                    k.lo &= after.lo;
                    k.hi &= after.hi;
                }
                if (k.lo != known[y].lo || k.hi != known[y].hi)
                {
                    known[y] = k;
                    changed = true;
                }
            }
        }
        return known;
    }

    std::string hex64(uint64_t v)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
        return buf;
    }

    // vu1_registry.cpp listing every vu1p_<signature>.cpp in dir.
    bool writeRegistry(const std::filesystem::path &dir)
    {
        std::set<std::string> names;
        for (const auto &entry : std::filesystem::directory_iterator(dir))
        {
            const std::string name = entry.path().filename().string();
            if (name.size() == 5u + 16u + 4u && name.rfind("vu1p_", 0) == 0 && name.substr(name.size() - 4) == ".cpp")
                names.insert(name.substr(5, 16));
        }
        std::FILE *f = std::fopen((dir / "vu1_registry.cpp").string().c_str(), "w");
        if (!f)
            return false;
        std::fprintf(f, "// Generated by ps2EntryRunner --vu-gen: every compiled VU1 program in this directory. Do not edit.\n");
        std::fprintf(f, "#include \"../ps2_vu1_compiled.h\"\n\n");
        for (const std::string &n : names)
            std::fprintf(f, "extern const VU1CompiledProgram vu1Program_%s;\n", n.c_str());
        std::fprintf(f, "\nconst VU1CompiledProgram *const g_vu1CompiledPrograms[] = {\n");
        for (const std::string &n : names)
            std::fprintf(f, "    &vu1Program_%s,\n", n.c_str());
        std::fprintf(f, "    nullptr,\n};\nconst size_t g_vu1CompiledProgramCount = %zu;\n", names.size());
        std::fclose(f);
        std::printf("[vugen] registry: %zu programs\n", names.size());
        return true;
    }
}

int vu1GeneratePrograms(const std::vector<std::vector<uint8_t>> &codes, std::vector<std::pair<uint32_t, uint32_t>> starts,
                        const std::vector<std::string> &sources, const char *outDir)
{
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    const std::filesystem::path dir(outDir);
    std::filesystem::create_directories(dir);

    uint32_t written = 0, existing = 0;
    std::set<uint64_t> done;
    ElisionStats stats;
    uint64_t normStats[4] = {}; // blocks BEGIN_Z, BEGIN_N, plain; pairs whose normalizations BEGIN_N changes
    for (const auto &[codeIndex, startPcBinding] : starts)
    {
        const uint32_t startPc = startPcBinding; // a lambda below uses it: clang 15 can't capture a binding
        const std::string &source = sources[codeIndex];
        const std::vector<uint8_t> &code = codes[codeIndex];
        const uint32_t codeSize = static_cast<uint32_t>(code.size());
        std::vector<uint32_t> reachable;
        const uint64_t signature = vu1ProgramSignature(code.data(), codeSize, startPc, &reachable);
        if (signature == 0u || !done.insert(signature).second)
            continue;
        const std::string name = hex64(signature);
        const std::filesystem::path file = dir / ("vu1p_" + name + ".cpp");
        if (std::filesystem::exists(file))
        {
            ++existing;
            continue;
        }
        // Pairs to compile (reserved encodings stay with the interpreter, which reports them).
        std::vector<std::array<uint32_t, 3>> compiled; // index, lower, upper
        for (uint32_t index : reachable)
        {
            uint32_t lower = 0, upper = 0;
            readPair(code.data(), index, lower, upper);
            const VU1Core::DecodedInstructionPair decoded = VU1Core::decodePairWords(lower, upper, VU1Core::Unit::VU1);
            if (!decoded.upperUsage.reserved && !decoded.lowerUsage.reserved)
                compiled.push_back({index, lower, upper});
        }
        const std::vector<PairElision> elisions = analyseElisions(code, compiled, stats);
        const std::vector<vu1detail::NormLanes> normEntry = analyseNormEntry(code, compiled, startPc);
        // Block leaders: the start, branch targets, the pair after a branch's or an E bit's delay slot
        // (JR/JALR targets are unknown; entering there just runs single pairs until the next leader).
        std::set<uint32_t> leaders{startPc / 8u};
        for (const auto &c : compiled)
        {
            const VU1Core::DecodedInstructionPair d = VU1Core::decodePairWords(c[1], c[2], VU1Core::Unit::VU1);
            const uint32_t pairs = codeSize / 8u;
            if (!d.iBit && d.lowerUsage.pipeline == VU1Core::PipelineBranch)
            {
                if ((d.lower >> 26) != 0x12u) // not JR/JALR
                {
                    int32_t imm = static_cast<int32_t>(d.lower & 0x7FFu);
                    if (imm & 0x400)
                        imm -= 0x800;
                    leaders.insert(static_cast<uint32_t>(static_cast<int32_t>(c[0] + 1u) + imm + static_cast<int32_t>(pairs)) % pairs);
                }
                leaders.insert((c[0] + 2u) % pairs);
            }
            if (d.eBit)
                leaders.insert((c[0] + 2u) % pairs);
        }
        constexpr size_t kMaxBlockPairs = 64; // measured: 64 is a little faster than 16 (nbh 8.8 -> 8.6 ns/pair)
        std::vector<std::array<uint32_t, 2>> blocks; // firstPos, count
        auto header = [&](std::FILE *out)
        {
            std::fprintf(out, "// Generated by ps2EntryRunner --vu-gen from %s. Do not edit; regenerate instead.\n", source.c_str());
            std::fprintf(out, "// VU1 program at pc 0x%04x: %zu reachable instruction pairs (%zu compiled), signature %s.\n",
                         startPc, reachable.size(), compiled.size(), name.c_str());
            std::fprintf(out, "#include \"../ps2_vu1_compiled.h\"\n\nnamespace vu1p_%s\n{\n", name.c_str());
        };
        // The pair functions go into chunk files of kChunkPairs each: every pair inlines the whole
        // step, so one file per big program needs minutes and gigabytes in a single compiler.
        constexpr size_t kChunkPairs = 48;
        for (size_t first = 0, chunk = 0; first < compiled.size(); first += kChunkPairs, ++chunk)
        {
            char suffix[16];
            std::snprintf(suffix, sizeof(suffix), "_%02zu.cpp", chunk);
            const std::filesystem::path chunkFile = dir / ("vu1p_" + name + suffix);
            std::FILE *out = std::fopen(chunkFile.string().c_str(), "w");
            if (!out)
            {
                std::fprintf(stderr, "[vugen] cannot write %s\n", chunkFile.string().c_str());
                return 1;
            }
            header(out);
            for (size_t i = first; i < std::min(compiled.size(), first + kChunkPairs); ++i)
            {
                const PairElision &e = elisions[i];
                if (e.elide == 0u)
                    std::fprintf(out, "VU1_COMPILED_PAIR(%u, 0x%08xu, 0x%08xu) // pc 0x%04x\n", compiled[i][0], compiled[i][1], compiled[i][2], compiled[i][0] * 8u);
                else
                    std::fprintf(out, "VU1_COMPILED_PAIR_S(%u, 0x%08xu, 0x%08xu, 0x%03xu, 0x%04xu, 0x%02xu, 0x%xu) // pc 0x%04x\n", compiled[i][0],
                                 compiled[i][1], compiled[i][2], e.elide, e.vfKeep, e.viKeep, e.accKeep, compiled[i][0] * 8u);
            }
            // Blocks of this chunk: consecutive pair indices, split at leaders.
            const size_t last = std::min(compiled.size(), first + kChunkPairs);
            for (size_t i = first; i < last;)
            {
                size_t j = i + 1;
                while (j < last && j - i < kMaxBlockPairs && compiled[j][0] == compiled[j - 1][0] + 1u && leaders.count(compiled[j][0]) == 0u)
                    ++j;
                if (j - i >= 2u)
                {
                    blocks.push_back({static_cast<uint32_t>(i), static_cast<uint32_t>(j - i)});
                    // Entry lanes: worth a BEGIN_N only where they change some pair's normalizations.
                    const vu1detail::NormLanes entryLanes = normEntry[compiled[i][0]];
                    uint32_t gained = 0;
                    vu1detail::NormLanes with = entryLanes, without{};
                    for (size_t k = i; k < j; ++k)
                    {
                        const VU1Core::DecodedInstructionPair d = VU1Core::decodePairWords(compiled[k][1], compiled[k][2], VU1Core::Unit::VU1);
                        gained += vu1detail::normPairBits(with, d) != vu1detail::normPairBits(without, d) ? 1u : 0u;
                        with = vu1detail::normAfterPair(with, d);
                        without = vu1detail::normAfterPair(without, d);
                    }
                    if ((entryLanes.lo | entryLanes.hi) == 0u)
                    {
                        std::fprintf(out, "\nVU1_COMPILED_BLOCK_BEGIN_Z(%u, 0x%sull)\n", compiled[i][0], name.c_str());
                        ++normStats[0];
                    }
                    else if (gained != 0u)
                    {
                        std::fprintf(out, "\nVU1_COMPILED_BLOCK_BEGIN_N(%u, 0x%sull, 0x%016llxull, 0x%016llxull)\n", compiled[i][0], name.c_str(),
                                     static_cast<unsigned long long>(entryLanes.lo), static_cast<unsigned long long>(entryLanes.hi));
                        ++normStats[1];
                        normStats[3] += gained;
                    }
                    else
                    {
                        std::fprintf(out, "\nVU1_COMPILED_BLOCK_BEGIN(%u)\n", compiled[i][0]);
                        ++normStats[2];
                    }
                    for (size_t k = i + 1; k < j; ++k)
                        std::fprintf(out, "VU1_COMPILED_BLOCK_NEXT(%u)\n", compiled[k][0]);
                    std::fprintf(out, "VU1_COMPILED_BLOCK_END()\n");
                }
                i = j;
            }
            std::fprintf(out, "} // namespace vu1p_%s\n", name.c_str());
            std::fclose(out);
        }
        // The program file, written last: its presence marks the program as generated.
        std::FILE *f = std::fopen(file.string().c_str(), "w");
        if (!f)
        {
            std::fprintf(stderr, "[vugen] cannot write %s\n", file.string().c_str());
            return 1;
        }
        header(f);
        for (const auto &pair : compiled)
            std::fprintf(f, "VU1_COMPILED_PAIR_DECL(%u)\n", pair[0]);
        std::fprintf(f, "\nconst VU1CompiledPair kPairs[] = {\n");
        for (const auto &pair : compiled)
            std::fprintf(f, "    {%uu, pair_%u},\n", pair[0], pair[0]);
        if (compiled.empty())
            std::fprintf(f, "    {0xFFFFFFFFu, nullptr},\n");
        std::fprintf(f, "};\n");
        for (const auto &block : blocks)
            std::fprintf(f, "VU1_COMPILED_BLOCK_DECL(%u)\n", compiled[block[0]][0]);
        std::fprintf(f, "const VU1CompiledBlock kBlocks[] = {\n");
        for (const auto &block : blocks)
            std::fprintf(f, "    {%uu, %uu, block_%u},\n", block[0], block[1], compiled[block[0]][0]);
        if (blocks.empty())
            std::fprintf(f, "    {0u, 0u, nullptr},\n");
        std::fprintf(f, "};\n} // namespace vu1p_%s\n\n", name.c_str());
        std::fprintf(f, "extern const VU1CompiledProgram vu1Program_%s;\n", name.c_str());
        std::fprintf(f, "const VU1CompiledProgram vu1Program_%s = {0x%sull, 0x%04xu, %zuu, vu1p_%s::kPairs, %zuu, vu1p_%s::kBlocks};\n",
                     name.c_str(), name.c_str(), startPc, compiled.size(), name.c_str(), blocks.size(), name.c_str());
        std::fclose(f);
        ++written;
        std::printf("[vugen] %s: pc 0x%04x, %zu pairs\n", file.filename().string().c_str(), startPc, compiled.size());
    }
    std::printf("[vugen] elisions over %llu pairs: marks upper %llu lower %llu vi %llu acc %llu; reads checked %llu of %llu (%llu pairs);"
                " regWritesUntil %llu, branch %llu, prev end %llu\n",
                (unsigned long long)stats.pairs, (unsigned long long)stats.upperMark, (unsigned long long)stats.lowerMark,
                (unsigned long long)stats.viMark, (unsigned long long)stats.accMark, (unsigned long long)stats.readsKept,
                (unsigned long long)stats.readsAll, (unsigned long long)stats.ready, (unsigned long long)stats.regUntil,
                (unsigned long long)stats.branch, (unsigned long long)stats.prevEnd);
    std::printf("[vugen] block entry lanes: %llu blocks set trust, %llu start with lanes known (%llu pairs normalize less), %llu plain\n",
                (unsigned long long)normStats[0], (unsigned long long)normStats[1], (unsigned long long)normStats[3], (unsigned long long)normStats[2]);
    std::printf("[vugen] %u new programs, %u already generated (%zu start points in %zu code images)\n", written, existing, starts.size(), codes.size());
    return writeRegistry(dir) ? 0 : 1;
}

// ---------------------------------------------------------------------------------------------
// ps2xVuGen <locations.toml> <extracted dir> <outDir>: the programs from the game's own files
// ---------------------------------------------------------------------------------------------
namespace
{
    struct DiscProgram
    {
        uint64_t signature = 0;
        uint32_t startPc = 0;
        std::string source;
        std::vector<std::array<uint64_t, 3>> code; // first pair, pair count, file offset
    };

    // The locations file: `key = value` lines, a [[program]] line per program. Top-level `file` and
    // `source` apply to every program; `code` lists [first pair, pair count, offset] triples.
    bool loadLocations(const char *path, std::string &file, std::vector<DiscProgram> &programs)
    {
        std::ifstream in(path);
        if (!in)
            return false;
        std::string source, line;
        while (std::getline(in, line))
        {
            if (const size_t hash = line.find('#'); hash != std::string::npos)
                line.erase(hash);
            if (line.find("[[program]]") != std::string::npos)
            {
                programs.emplace_back().source = source;
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string key = line.substr(0, eq), value = line.substr(eq + 1);
            key.erase(std::remove_if(key.begin(), key.end(), [](char c) { return c == ' ' || c == '\t'; }), key.end());
            const size_t q0 = value.find('"'), q1 = value.rfind('"');
            const std::string text = q0 != q1 ? value.substr(q0 + 1, q1 - q0 - 1) : std::string();
            if (key == "file")
                file = text;
            else if (key == "source")
                (programs.empty() ? source : programs.back().source) = text;
            else if (programs.empty())
                continue;
            else if (key == "signature")
                programs.back().signature = std::strtoull(value.c_str(), nullptr, 0);
            else if (key == "start_pc")
                programs.back().startPc = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 0));
            else if (key == "code")
            {
                std::vector<uint64_t> numbers;
                for (const char *p = value.c_str(); *p;)
                {
                    if (*p < '0' || *p > '9')
                    {
                        ++p;
                        continue;
                    }
                    char *end = nullptr;
                    numbers.push_back(std::strtoull(p, &end, 0));
                    p = end;
                }
                for (size_t i = 0; i + 2 < numbers.size(); i += 3)
                    programs.back().code.push_back({numbers[i], numbers[i + 1], numbers[i + 2]});
            }
        }
        return !file.empty() && !programs.empty();
    }
}

int ps2xVuGenerateDisc(const char *locationsPath, const char *extractedDir, const char *outDir)
{
    std::string fileName;
    std::vector<DiscProgram> programs;
    if (!loadLocations(locationsPath, fileName, programs))
    {
        std::fprintf(stderr, "[vugen] cannot read VU1 program locations %s\n", locationsPath);
        return 1;
    }
    const std::filesystem::path filePath = std::filesystem::path(extractedDir) / fileName;
    std::ifstream in(filePath, std::ios::binary);
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (file.empty())
    {
        std::fprintf(stderr, "[vugen] cannot read %s\n", filePath.string().c_str());
        return 1;
    }
    // One 16 KB code image per program, holding only its own pairs (the rest of VU1 micro memory
    // never enters the signature or the generated code).
    std::vector<std::vector<uint8_t>> codes;
    std::vector<std::pair<uint32_t, uint32_t>> starts;
    std::vector<std::string> sources;
    for (const DiscProgram &program : programs)
    {
        std::vector<uint8_t> code(kMaxPairs * 8u, 0u);
        for (const auto &[first, count, offset] : program.code)
        {
            if (first + count > kMaxPairs || offset + count * 8u > file.size())
            {
                std::fprintf(stderr, "[vugen] program %016llx: pairs %llu+%llu at 0x%llx are outside %s or VU1 memory\n",
                             (unsigned long long)program.signature, (unsigned long long)first, (unsigned long long)count,
                             (unsigned long long)offset, fileName.c_str());
                return 1;
            }
            std::memcpy(code.data() + first * 8u, file.data() + offset, count * 8u);
        }
        const uint64_t signature = vu1ProgramSignature(code.data(), static_cast<uint32_t>(code.size()), program.startPc, nullptr);
        if (signature != program.signature)
        {
            std::fprintf(stderr, "[vugen] program %016llx (pc 0x%04x) not found in %s (signature %016llx): a different version of the game?\n",
                         (unsigned long long)program.signature, program.startPc, fileName.c_str(), (unsigned long long)signature);
            return 1;
        }
        starts.push_back({static_cast<uint32_t>(codes.size()), program.startPc});
        codes.push_back(std::move(code));
        sources.push_back(program.source);
    }
    std::printf("[vugen] %zu VU1 programs read from %s\n", programs.size(), fileName.c_str());
    return vu1GeneratePrograms(codes, std::move(starts), sources, outDir);
}
