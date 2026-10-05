// KartTesterLoader.cc
//
// ONE RACE, THREE RUNTIMES' WORTH OF IT, IN ONE PROCESS. Three worlds built by
// the same course script (KartProvider/scripts/kart_course.etcs), three races
// on them: the host's judges, two guests follow its record. What a link
// carries between runtimes is carried here by local Ledgers and the same
// stream scripts the page runs: each guest's proposals ledger is authored as
// that guest (what a link's name does), the host judges both, and both guests
// follow the host's record. What is held to account:
//
//   1. THE GRID. Three drivers, three karts, each on the grid in its colour.
//   2. THE PEDAL. W takes the host's kart up the straight toward top speed.
//   3. THE WHEEL. A turns the wheels left, and a moving kart turns with them;
//      a kart standing still with its wheels turned does not turn at all, and
//      its front wheels are seen turned while the rear stay square.
//   4. THE WALL. A kart driven at the end wall stops there: solid meets solid.
//   5. A LAP. A guest steered round by a simple driver crosses the half-way
//      gate and the line, and the race counts it, with a time.
//   6. THE SAME RACE. Each guest at the host's tick has the host's hash -- the
//      karts' rows to the bit -- and the same snapshot to advertise.
//
//   ./Run_KartTesterLoader
//
// No display, no GPU, no network.

#include "../ETCS.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static std::string verb(ETCS::Entity* e, const std::string& action, const std::string& args = "")
{
    ETCS::Buffer a(action.c_str()), d(args.c_str());
    e->call(a, d);
    return d.toString();
}

struct Kart { int slot = -1; float x = 0, z = 0, hx = 0, hz = 0, speed = 0; };
static Kart where(ETCS::Entity* race, const std::string& who)
{
    Kart k;
    std::istringstream in(verb(race, "KartRace.Kart", who));
    in >> k.slot >> k.x >> k.z >> k.hx >> k.hz >> k.speed;
    return k;
}
static uint64_t tickOf(ETCS::Entity* race)
{
    std::istringstream in(verb(race, "KartRace.Status"));
    uint64_t t = 0; in >> t; return t;
}

int main(int, char**)
{
    WIRE_CONTEXT();
    std::cout << "=== One race, three runtimes' worth ===\n";

    // Three worlds and three races, each built by the course.
    ETCS::Entity* race[3] = {};
    ETCS::Entity* worlds[3] = {};
    for (int i = 0; i < 3; ++i)
    {
        ETCS::Entity* world = worlds[i] = ETCS::spawn_entity("RenderProvider", "Scene3D", env, loader);
        race[i] = ETCS::spawn_entity("KartProvider", "KartRace", env, loader);
        if (!world || !race[i]) { std::printf("FAILED (could not spawn)\n"); return 1; }
        verb(world, "Scene3D.Create", "1, 1, 1");
        verb(world, "Scene3D.SetVisible", "0");
        verb(race[i], "KartRace.Create");
        verb(race[i], "KartRace.BindWorld", std::to_string(world->getRID()));
        ETCS::ExecutionContext course(&root, &ctx);
        course.names["anchor"] = ETCS::NameBinding{ world->getRID(), "RenderProvider", "Scene3D" };
        course.names["race"]   = ETCS::NameBinding{ race[i]->getRID(), "KartProvider", "KartRace" };
        // Run BY a script, as a page's boot runs it: its names are its own,
        // so three courses in one process do not take each other's.
        std::istringstream in("run KartProvider/scripts/kart_course.etcs anchor=anchor race=race\n");
        const std::string origin = std::string(ETCS_ACE_ROOT) + "/modules/kart_world.etcs";
        if (!ETCS::run_script(in, origin, course)) { std::printf("FAILED (the course script)\n"); return 1; }
    }
    ETCS::Entity* host = race[0];
    ETCS::Entity* g1 = race[1];
    ETCS::Entity* g2 = race[2];

    // The record and a proposals ledger per guest, wired as the page wires
    // them -- with the page's own stream scripts.
    ETCS::Entity* rec = ETCS::spawn_entity("NetworkProvider", "Ledger", env, loader);
    ETCS::Entity* p1  = ETCS::spawn_entity("NetworkProvider", "Ledger", env, loader);
    ETCS::Entity* p2  = ETCS::spawn_entity("NetworkProvider", "Ledger", env, loader);
    verb(rec, "Ledger.Author", "host");
    verb(p1, "Ledger.Author", "g1");
    verb(p2, "Ledger.Author", "g2");
    verb(host, "KartRace.Host", std::to_string(rec->getRID()) + " " + std::to_string(p1->getRID()) + " 0 host 0 t1");
    verb(g1, "KartRace.Join", "0 g1 host");
    verb(g2, "KartRace.Join", "0 g2 host");
    {
        ETCS::ExecutionContext wire(&root, &ctx);
        auto bind = [&](const char* n, ETCS::Entity* e, const char* mod, const char* tag)
        { wire.names[n] = ETCS::NameBinding{ e->getRID(), mod, tag }; };
        bind("rec", rec, "NetworkProvider", "Ledger"); bind("p1", p1, "NetworkProvider", "Ledger"); bind("p2", p2, "NetworkProvider", "Ledger");
        bind("host", host, "KartProvider", "KartRace"); bind("g1", g1, "KartProvider", "KartRace"); bind("g2", g2, "KartProvider", "KartRace");
        std::istringstream lines(
            "detach KartProvider/scripts/kart_judge.etcs src=p1 dst=host\n"
            "detach KartProvider/scripts/kart_judge.etcs src=p2 dst=host\n"
            "detach KartProvider/scripts/kart_follow.etcs src=rec dst=g1\n"
            "detach KartProvider/scripts/kart_follow.etcs src=rec dst=g2\n"
            "detach KartProvider/scripts/kart_emit.etcs src=g1 dst=p1\n"
            "detach KartProvider/scripts/kart_emit.etcs src=g2 dst=p2\n");
        const std::string origin = std::string(ETCS_ACE_ROOT) + "/modules/kart_wiring.etcs";
        check(ETCS::run_script(lines, origin, wire), "the streams are wired with the page's scripts");
    }
    // The host's clock, by hand, and the guests let catch up with it.
    auto settle = [&]() {
        const uint64_t t = tickOf(host);
        for (int i = 0; i < 400 && (tickOf(g1) != t || tickOf(g2) != t); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    auto run = [&](uint32_t ticks) { verb(host, "KartRace.Run", std::to_string(ticks)); settle(); };
    auto key = [&](ETCS::Entity* r, char k, bool down) {
        verb(r, "KartRace.Key", std::to_string(static_cast<int>(k)) + " " + (down ? "1" : "0"));
    };
    // A guest's key reaches the host by the proposals stream: wait for it to
    // be judged (the host's record seq moves) before the clock runs on.
    auto seqOf = [&](ETCS::Entity* r) { std::istringstream in(verb(r, "KartRace.Status")); uint64_t t = 0, s = 0; in >> t >> s; return s; };
    auto guestKey = [&](ETCS::Entity* r, char k, bool down) {
        const uint64_t before = seqOf(host);
        key(r, k, down);
        for (int i = 0; i < 400 && seqOf(host) == before; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };

    // -- 1. the grid ---------------------------------------------------------
    std::cout << "\n-- 1. the grid --\n";
    for (int i = 0; i < 200; ++i)
    {
        run(2);
        std::string s = verb(host, "KartRace.Standings");
        if (std::count(s.begin(), s.end(), '\n') >= 3) break;
    }
    const std::string st = verb(host, "KartRace.Standings");
    check(std::count(st.begin(), st.end(), '\n') == 3, "three drivers, three karts");
    const Kart h0 = where(host, "host"), a0 = where(host, "g1"), b0 = where(host, "g2");
    check(h0.slot == 0 && a0.slot >= 0 && b0.slot >= 0 && a0.slot != b0.slot, "each in a kart of their own");
    check(h0.x < 0.0f && h0.z < -8.0f && std::fabs(h0.hx - 1.0f) < 1e-3f && h0.speed == 0.0f,
          "the host on the grid, behind the line, facing up the straight, still");

    // -- 2. the pedal --------------------------------------------------------
    std::cout << "\n-- 2. the pedal --\n";
    key(host, 'W', true);
    run(100);
    const Kart h1 = where(host, "host");
    check(h1.x - h0.x > 10.0f && h1.speed > 10.0f && std::fabs(h1.z - h0.z) < 0.05f,
          "W: up the straight, quickly, in a straight line");

    // -- 3. the wheel --------------------------------------------------------
    std::cout << "\n-- 3. the wheel --\n";
    key(host, 'A', true);
    run(25);
    const Kart h2 = where(host, "host");
    key(host, 'A', false);
    key(host, 'W', false);
    check(h2.hz < -0.3f && h2.z < h1.z, "A while moving: the wheels turn left, and the kart with them");
    guestKey(g2, 'A', true);
    run(50);
    const Kart b1 = where(host, "g2");
    guestKey(g2, 'A', false);
    check(std::fabs(b1.hx - 1.0f) < 1e-3f && b1.speed == 0.0f, "A standing still: the wheels turn, the kart does not");
    {
        // The wheels seen: g2's four, found as the drawn things at its kart's
        // corners, each axle (its cylinder's axis) across the kart -- the
        // front two turned left by the lock, the rear two square.
        Causal_* wc = static_cast<Causal_*>(worlds[0]->getInterfacePointer(ETCS::Buffer("Causal")));
        std::vector<Causal_*> near;
        wc->Near(Fixed::From(b1.x), Fixed::From(0.25), Fixed::From(b1.z), Fixed::From(1.4), near);
        float front = 0, rear = 0; int nf = 0, nr = 0;
        for (Causal_* c : near)
        {
            std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
            const OrderVector& r = c->Order4();
            // A wheel by its size: a reach of |(0.23, 0.12, 0.23)|. The kart,
            // the ground and the marks near it are all other sizes.
            if (c->Solid().shape != CausalSolid::Off || std::fabs(r.radius.ToFloat() - 0.347f) > 0.01f) continue;
            Fixed ax = Fixed::Zero(), ay = Fixed::One(), az = Fixed::Zero();
            r.RotateVector(ax, ay, az);
            // How far the axle is off the kart's own across (hz, -hx), either end.
            const float dot = std::fabs(ax.ToFloat() * b1.hz - az.ToFloat() * b1.hx);
            const float a = std::acos(dot > 1.0f ? 1.0f : dot);
            const float along = (r.x.ToFloat() - b1.x) * b1.hx + (r.z.ToFloat() - b1.z) * b1.hz;
            if (along > 0) { front += a; ++nf; } else { rear += a; ++nr; }
        }
        std::printf("        (axles off square: front %.3f rad, rear %.3f rad, %d+%d wheels)\n", nf ? front / nf : -1.f, nr ? rear / nr : -1.f, nf, nr);
        check(nf == 2 && nr == 2 && std::fabs(front / nf - 0.45f) < 0.02f && rear / nr < 0.01f,
              "...and the front wheels are seen turned to the lock, the rear ones square");
    }

    // -- 4. the wall ---------------------------------------------------------
    std::cout << "\n-- 4. the wall --\n";
    guestKey(g1, 'W', true);
    run(400);
    const Kart a1 = where(host, "g1");
    guestKey(g1, 'W', false);
    check(a1.x > 25.0f && a1.x < 29.1f, "a kart driven at the end wall stops at it: solid meets solid");
    run(100);

    // -- 5. a lap ------------------------------------------------------------
    std::cout << "\n-- 5. a lap --\n";
    // A driver: the pedal down, the wheel toward the next corner of the loop.
    const float way[][2] = { { 22, -14 }, { 24, 12 }, { -22, 14 }, { -24, -12 }, { 6, -14 } };
    size_t next = 0;
    int wheel = 0;
    guestKey(g2, 'W', true);
    for (int i = 0; i < 700 && next < 5; ++i)
    {
        const Kart b = where(host, "g2");
        const float dx = way[next][0] - b.x, dz = way[next][1] - b.z;
        if (dx * dx + dz * dz < 36.0f) { ++next; continue; }
        const float l = std::sqrt(dx * dx + dz * dz);
        const float left = (dx * b.hz - dz * b.hx) / l;   // (dx,dz) . (hz,-hx)
        const int want = left > 0.08f ? 1 : (left < -0.08f ? -1 : 0);
        if (want != wheel)
        {
            if (wheel == 1) guestKey(g2, 'A', false);
            if (wheel == -1) guestKey(g2, 'D', false);
            if (want == 1) guestKey(g2, 'A', true);
            if (want == -1) guestKey(g2, 'D', true);
            wheel = want;
        }
        run(5);
    }
    run(10);
    std::istringstream sl(verb(host, "KartRace.Standings"));
    std::string row; int laps = -1; std::string best;
    while (std::getline(sl, row))
    {
        std::istringstream rs(row);
        int slot; std::string who, last; int n;
        rs >> slot >> who >> n >> last >> best;
        if (who == "g2") { laps = n; break; }
    }
    std::printf("        (g2: %d lap(s), best %s s, %zu of 5 corners)\n", laps, best.c_str(), next);
    check(laps == 1 && best != "-", "steered round, past the half-way gate and over the line: one lap, timed");

    // -- 6. the same race ----------------------------------------------------
    std::cout << "\n-- 6. the same race --\n";
    run(2);
    const std::string hh = verb(host, "KartRace.Hash"), h1s = verb(g1, "KartRace.Hash"), h2s = verb(g2, "KartRace.Hash");
    std::printf("        (host %s / g1 %s / g2 %s)\n", hh.c_str(), h1s.c_str(), h2s.c_str());
    check(hh == h1s && hh == h2s, "each guest at the host's tick is the host's race, to the bit");
    const std::string sh = verb(host, "KartRace.Snapshot");
    check(sh == verb(g1, "KartRace.Snapshot") && sh == verb(g2, "KartRace.Snapshot") && sh.compare(0, 2, "0 ") != 0,
          "...and advertises the same snapshot");

    // The streams wound up as a closing script winds up what it detached:
    // each told to stop, then joined.
    auto& detached = ETCS::DetachedRegistry::getInstance();
    for (auto& [id, script] : detached.list()) detached.interrupt(id);
    for (int i = 0; i < 2500 && !detached.all_finished(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(detached.all_finished(), "the six streams end when told to");
    detached.join_all();
    for (ETCS::Entity* r : race) verb(r, "KartRace.Delete");
    ETCS::PendingUnloadRegistry::getInstance().join_all();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
