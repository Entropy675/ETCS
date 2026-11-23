// KartMapsTesterLoader.cc
//
// THE KART'S MAP GENERATOR, OVER MANY SEEDS. A map from a seed is one nobody
// looked at before it is played on, so what a hand-made map's author checks
// by eye the generator has to get right for every seed. This makes the map of
// each of many seeds -- KartArena::Generate itself, compiled in from the
// module's header, no battle and no runtime -- and holds every one to account:
//
//   1. SOUND. None has a fault (KartArena::Faults): every pad and spring on
//      level ground it can be reached on, room under every deck, every pit
//      clear at its edges with a way out.
//   2. FURNISHED. Every one has its eight spawns, a deck, two springs or
//      more, a pad, a second box, and three or more of blocks, ramps and
//      hills; about one in two has a pit.
//   3. BIG LEVELS. The storey is over a tenth of the arena or more; the low
//      ground, where there is any, is a third of it or more, with two ramps
//      out or more.
//   4. THE SAME AGAIN. A seed makes the same map twice.
//
// It also prints how much of each thing a map has on average and at most: the
// numbers to look at after a change to the generator. A few layouts seen in
// KartTester say nothing about the one seed in six that goes wrong.
//
//   ./Run_KartMapsTesterLoader [<seeds>]      (20000)
//
// The header is KartProvider's own, not its tag surface: the loader's
// manifest (ACE-Build-Tools manifests/loaders/KartMapsTesterLoader.loader.json)
// inherits the module.

#include "../ETCS.h"
#include "../modules/KartProvider/KartProvider/KartArena.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

// Every number on a map, in order: two maps are the same when these are.
static std::string numbers(const KartArena::Map& m)
{
    std::string st;
    auto put = [&st](const Fixed& f) { st += std::to_string(f.raw) + " "; };
    for (const auto& b : m.blocks)  { put(b.x); put(b.z); put(b.w); put(b.d); put(b.h); st += std::to_string(b.yaw) + "|"; }
    for (const auto& r : m.ramps)   { put(r.x); put(r.z); put(r.width); put(r.up); put(r.flat); put(r.down); put(r.height); st += std::to_string(r.yaw) + "|"; }
    for (const auto& k : m.hills)   { put(k.x); put(k.z); put(k.radius); put(k.height); st += "|"; }
    for (const auto& p : m.pads)    { put(p.x); put(p.z); st += std::to_string(p.yaw) + "|"; }
    for (const auto& p : m.springs) { put(p.x); put(p.z); st += "|"; }
    for (const auto& d : m.decks)   { put(d.x); put(d.z); put(d.w); put(d.d); put(d.h); st += "|"; }
    for (const auto& p : m.pits)    { put(p.x); put(p.z); put(p.w); put(p.d); put(p.depth); st += "|"; }
    for (const auto& p : m.spawns)  { put(p.x); put(p.z); }
    for (const auto& p : m.pickups) { put(p.x); put(p.z); if (p.up) st += "u"; }
    return st;
}

int main(int argc, char** argv)
{
    const int seeds = argc > 1 ? std::atoi(argv[1]) : 20000;
    if (seeds <= 0) { std::printf("FAILED (seeds: %s)\n", argv[1]); return 1; }
    std::printf("-- the kart's map generator, %d seeds --\n", seeds);

    // What a map has, by kind: how many maps have none, the sum, the most.
    static const char* const kinds[] = { "blocks", "ramps", "hills", "pads", "springs", "decks", "pits", "boxes" };
    enum { Kinds = sizeof kinds / sizeof kinds[0] };
    long none[Kinds] = {}, sum[Kinds] = {}, most[Kinds] = {};
    int faulty = 0, bare = 0, differs = 0, small = 0;
    long climbs = 0, stairless = 0, exits = 0;
    uint64_t dice = 0x9e3779b97f4a7c15ull;
    for (int i = 0; i < seeds; ++i)
    {
        const uint64_t seed = KartArms::Next(dice);
        KartArena::Map m;
        KartArena::Generate(seed, m);
        const size_t has[Kinds] = { m.blocks.size(), m.ramps.size(), m.hills.size(), m.pads.size(),
                                    m.springs.size(), m.decks.size(), m.pits.size(), m.pickups.size() };
        for (int k = 0; k < Kinds; ++k)
        {
            none[k] += has[k] == 0;
            sum[k] += static_cast<long>(has[k]);
            if (static_cast<long>(has[k]) > most[k]) most[k] = static_cast<long>(has[k]);
        }
        // The levels: how much of the arena each takes, the ramps up to the
        // storey and the ramps out of the low ground.
        const double side = static_cast<double>(KartArena::Half().ToFloat()) * 2.0, arena = side * side;
        double storey = 0.0, low = 0.0;
        for (const auto& d : m.decks) storey += static_cast<double>((d.w * d.d).ToFloat()) / arena;
        for (const auto& p : m.pits)  low += static_cast<double>((p.w * p.d).ToFloat()) / arena;
        long up = 0, out = 0;
        for (const auto& a : m.ramps)
        {
            up += a.height == KartArena::StoreyHeight();
            out += !m.pits.empty() && a.base.raw < 0 && a.height == KartArena::LowDepth();
        }
        climbs += up; stairless += up == 0; exits += out;
        const bool big = storey >= 0.09 && (m.pits.empty() || (low >= 0.3 && out >= 2));
        if (!big && small++ < 5)
            std::printf("        seed %llu: a storey over %.0f%%, low ground %.0f%% with %ld ramps out\n",
                        static_cast<unsigned long long>(seed), storey * 100.0, low * 100.0, out);
        const std::string faults = KartArena::Faults(m);
        if (!faults.empty() && faulty++ < 5) std::printf("        seed %llu:\n%s\n", static_cast<unsigned long long>(seed), faults.c_str());
        const bool furnished = m.spawns.size() == 8 && !m.decks.empty() && m.springs.size() >= 2 && !m.pads.empty()
                            && m.pickups.size() >= 2 && m.blocks.size() + m.ramps.size() + m.hills.size() >= 3;
        if (!furnished && bare++ < 5)
            std::printf("        seed %llu: %zu spawns, %zu decks, %zu springs, %zu pads, %zu boxes, %zu blocks, %zu ramps, %zu hills\n",
                        static_cast<unsigned long long>(seed), m.spawns.size(), m.decks.size(), m.springs.size(), m.pads.size(),
                        m.pickups.size(), m.blocks.size(), m.ramps.size(), m.hills.size());
        if (i % 100 == 0)   // the same again, one seed in a hundred
        {
            KartArena::Map again;
            KartArena::Generate(seed, again);
            differs += numbers(again) != numbers(m);
        }
    }
    for (int k = 0; k < Kinds; ++k)
        std::printf("        %-8s %5.2f a map, at most %ld, none on %ld (%.1f%%)\n", kinds[k],
                    static_cast<double>(sum[k]) / seeds, most[k], none[k], 100.0 * static_cast<double>(none[k]) / seeds);
    std::printf("        ramps up to the storey %.2f a map, none on %ld (%.1f%%); ramps out of the low ground %.2f where there is any\n",
                static_cast<double>(climbs) / seeds, stairless, 100.0 * static_cast<double>(stairless) / seeds,
                seeds > none[6] ? static_cast<double>(exits) / static_cast<double>(seeds - none[6]) : 0.0);
    std::printf("        (%d with a fault, %d short of something, %d with a level too small)\n", faulty, bare, small);
    check(faulty == 0, "every map is sound: nothing on it where it cannot be reached or used");
    check(bare == 0, "every map has eight spawns, a deck, two springs, a pad, a second box and three things to drive round");
    check(small == 0, "every storey is over a tenth of the arena or more; low ground is a third or more, with two ramps out or more");
    const double pits = static_cast<double>(seeds - none[6]) / seeds;
    check(seeds < 1000 || (pits > 0.4 && pits < 0.6), "about one in two has a pit");
    check(differs == 0, "a seed makes the same map twice");

    // A bank by hand, up to the north wall: its lip half a unit short of the
    // karts' bound leaves a trench under it, which is a fault; on the bound
    // there is none (the bowl's banks were the first, the skatepark's too).
    auto bank = [](float middle) {
        KartArena::Map m;
        KartArena::Ramp r;
        r.x = Fixed::Zero(); r.z = Fixed::From(middle); r.yaw = 0;
        r.width = Fixed::FromInt(192); r.up = Fixed::FromInt(21); r.flat = r.down = Fixed::Zero(); r.height = Fixed::FromInt(3);
        m.ramps.push_back(r);
        KartArena::Prepare(m);
        return KartArena::Faults(m);
    };
    const std::string shy = bank(84.0f), flush = bank(84.5f);
    std::printf("        (a bank to 94.5: %s; to 95: %s)\n", shy.empty() ? "sound" : shy.c_str(), flush.empty() ? "sound" : flush.c_str());
    check(shy.find("a trench between its lip and the wall") != std::string::npos, "a bank whose lip is short of the karts' bound is a fault: a trench to hide in");
    check(flush.find("trench") == std::string::npos, "...and with its lip on the bound there is none (this map of one bank is bare, and is told that instead)");

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
