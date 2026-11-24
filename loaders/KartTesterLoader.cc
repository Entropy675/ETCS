// KartTesterLoader.cc
//
// ONE BATTLE, THREE RUNTIMES' WORTH OF IT, IN ONE PROCESS. Three worlds built
// by the same arena script (KartProvider/scripts/kart_arena.etcs), three
// battles on them: the host's is the authority, two guests mirror its trace.
// What a link carries between runtimes is carried here by the same stream
// scripts the page runs, between the battles directly: each guest's seat
// into the host's battle (the stream names the seat), and the host's trace
// out to each guest. What is held to account:
//
//   0. THE MAPS. Each hand-made one is sound (KartArena::Faults): what is on
//      it can be reached and does what it shows. A map with a fault is told
//      so. (A map from a seed is held to the same over many seeds by
//      KartMapsTesterLoader.)
//   1. THE LOBBY. Three drivers, three karts, each at the spawn furthest from
//      the others, facing the middle, whole and unarmed -- and parked: W moves
//      nobody, and a guest sets nothing.
//   2. A ROUND. Started, its settings are locked. The pedal takes the host
//      toward the middle, down a ramp into the bowl's low ground, and a
//      bumper there stops it. A turns the wheels left -- to the lock at once,
//      a tap no further, and held, on to twice the lock -- and let go they
//      come back; inverted steering swaps the keys for the runtime that set
//      it. A kart standing still does not turn, its front wheels (its own
//      members) seen turned and the rear square. The host abandons it: back
//      to the lobby.
//   3. THE SETTINGS. In the lobby the host's `config` brings out another map
//      at once and respawns everyone on it.
//   4. A ROUND ABANDONED WITH A SCORE, ONE COMPLETED. An elimination in a
//      round the host abandons is discarded with it. Speed builds gradually
//      and in a straight line the tyres roll and mark nothing; braked to a
//      stop every tyre leaves a dark mark, kept while it stands and while the
//      road since is short of forty units, then wiped from the far end; at
//      speed D turns the wheels right, at once to a smaller lock and on
//      past it while held, an arc that tightens -- more than the tyres hold,
//      so it drifts, keeping nearly all its speed, its tyres hot and marking
//      the road. Then: the sniper dead on takes a whole kart,
//      back three seconds later. A rocket bursts on a kart for up to 40; a
//      fuse hurts all but its own kart; a pad pushes a kart over it the way
//      it points; a power-up hands over a weapon; the minigun fires while
//      held, a tap between two ticks still fires, a boost outruns a kart's
//      top. The clock runs out into the results, then the lobby, which keeps
//      the round's scores; the next round starts them afresh.
//   5. THE GROUND. On the skatepark a kart climbs a ramp at the ramp's
//      height, flies off its lip and lands beyond it, and backing off over
//      the marks its brake left leaves them as they were laid; another driven
//      at a lip from the drop side is stopped by it.
//   6. A MAP FROM A SEED. The same layout on every runtime; a new seed, a new
//      layout, still the same everywhere, with pads, springs and a deck;
//      every kart on the ground at a spawn.
//   7. THE LEVELS, AND THE EYE. On a course of the test's own: the eye rides
//      behind its kart, swings round it when looked round (a verb, or Q and E)
//      and drifts back, slowly; a kart below a deck drives under it, its eye
//      with it, and on up a long ramp onto a storey; one driven into a pit
//      falls to its floor, its eye down there with it, and climbs the ramp
//      out; a spring throws one up -- higher the faster it comes at it --
//      it comes down on a deck in the air, and off the deck's edge it falls
//      back to the floor.
//   8. THE EDGES. On a map of the test's own: a kart driven into lava is
//      out; a spring taken at speed throws a kart over the arena's wall, and
//      out there it falls and is out; a pad on a bank's slope pushes a kart
//      up it. And on another, cover: a rocket into a block bursts within its
//      reach of the kart behind it, and the block keeps the fire off.
//   9. THE SAME BATTLE. Each guest at the host's tick has the host's hash --
//      the karts' rows, the shots, the dice and the scores, to the bit -- and
//      the same snapshot to advertise.
//
//   ./Run_KartTesterLoader
//
// No display, no GPU, no network.

#include "../ETCS.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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

// KartBattle.Kart: "<slot> <x> <z> <hx> <hz> <speed> <hp> <weapon> <ammo> <y> <air|ground> <wheel> <slide> <spare> <spare ammo> <oil ticks> <dagger ticks>".
struct Kart { int slot = -1; float x = 0, z = 0, hx = 0, hz = 0, speed = 0; int hp = -1; std::string weapon; int ammo = 0; float y = 0; std::string air; float wheel = 0, slide = 0; std::string spare; int spare_ammo = 0, slip = 0, dagger = 0; };
static Kart where(ETCS::Entity* b, const std::string& who)
{
    Kart k;
    const std::string row = verb(b, "KartBattle.Kart", who);
    if (row == "-") return k;   // not driving (a failed >> would read a 0)
    std::istringstream in(row);
    in >> k.slot >> k.x >> k.z >> k.hx >> k.hz >> k.speed >> k.hp >> k.weapon >> k.ammo >> k.y >> k.air >> k.wheel >> k.slide >> k.spare >> k.spare_ammo >> k.slip >> k.dagger;
    return k;
}
// KartBattle.Tyres, per tyre (fl fr rl rr): "<heat> <rope> <shown> <darkest>".
struct Tyre { float heat = 0, rope = 0; int shown = 0; float darkest = 0; };
struct Tyres { Tyre t[4]; int shown() const { return t[0].shown + t[1].shown + t[2].shown + t[3].shown; } };
static Tyres tyres(ETCS::Entity* b, const std::string& who)
{
    Tyres r;
    std::istringstream in(verb(b, "KartBattle.Tyres", who));
    for (Tyre& t : r.t) in >> t.heat >> t.rope >> t.shown >> t.darkest;
    return r;
}
// KartBattle.Status: "<tick> <lines> <mode> <me> <slot> <phase> <secs left> <map> <round secs> <layout>".
struct Status { uint64_t tick = 0, lines = 0; std::string mode, me; int slot = -1; std::string phase; int left = 0; std::string map; int secs = 0; uint64_t layout = 0; };
static Status status(ETCS::Entity* b)
{
    Status s;
    std::istringstream in(verb(b, "KartBattle.Status"));
    in >> s.tick >> s.lines >> s.mode >> s.me >> s.slot >> s.phase >> s.left >> s.map >> s.secs >> s.layout;
    return s;
}
// KartBattle.Scores, one driver's row: "<slot> <driver> <elims> <deaths> <hp> <weapon> <ammo> <alive|out>".
struct Score { int elims = -1, deaths = -1, hp = -1; std::string weapon, state; };
static Score score(ETCS::Entity* b, const std::string& who)
{
    std::istringstream all(verb(b, "KartBattle.Scores"));
    std::string row;
    while (std::getline(all, row))
    {
        std::istringstream rs(row);
        Score s; int slot, ammo; std::string driver;
        rs >> slot >> driver >> s.elims >> s.deaths >> s.hp >> s.weapon >> ammo >> s.state;
        if (driver == who) return s;
    }
    return Score{};
}
static bool near(float a, float b, float tol) { return std::fabs(a - b) < tol; }

int main(int, char**)
{
    WIRE_CONTEXT();
    std::cout << "=== One battle, three runtimes' worth ===\n";

    // Three worlds and three battles, each built by the arena.
    ETCS::Entity* battle[3] = {};
    ETCS::Entity* worlds[3] = {};
    for (int i = 0; i < 3; ++i)
    {
        ETCS::Entity* world = worlds[i] = ETCS::spawn_entity("RenderProvider", "Scene3D", env, loader);
        battle[i] = ETCS::spawn_entity("KartProvider", "KartBattle", env, loader);
        if (!world || !battle[i]) { std::printf("FAILED (could not spawn)\n"); return 1; }
        verb(world, "Scene3D.Create", "1, 1, 1");
        verb(world, "Scene3D.SetVisible", "0");
        verb(battle[i], "KartBattle.Create");
        verb(battle[i], "KartBattle.BindWorld", std::to_string(world->getRID()));
        ETCS::ExecutionContext arena(&root, &ctx);
        arena.names["anchor"] = ETCS::NameBinding{ world->getRID(), "RenderProvider", "Scene3D" };
        arena.names["battle"] = ETCS::NameBinding{ battle[i]->getRID(), "KartProvider", "KartBattle" };
        // Run BY a script, as a page's boot runs it: its names are its own,
        // so three arenas in one process do not take each other's.
        std::istringstream in("run KartProvider/scripts/kart_arena.etcs anchor=anchor battle=battle\n");
        const std::string origin = std::string(ETCS_ACE_ROOT) + "/modules/kart_world.etcs";
        if (!ETCS::run_script(in, origin, arena)) { std::printf("FAILED (the arena script)\n"); return 1; }
    }
    ETCS::Entity* host = battle[0];
    ETCS::Entity* g1 = battle[1];
    ETCS::Entity* g2 = battle[2];
    check(verb(host, "KartBattle.Maps") == "bowl\npillars\ncrossroads\nhills\nskatepark\nlava\ntowers\nserpent\nrandom\n",
          "the arena has its nine maps, in order");
    {
        const std::string faults = verb(host, "KartBattle.Faults");
        if (faults != "none") std::printf("%s\n", faults.c_str());
        check(faults == "none", "...each of the hand-made ones sound: what is on it can be reached and used");
    }
    // A seventh, of this test's own, the same on every runtime: three lanes
    // to the middle. The host's has a spring and then a deck in the air;
    // g1's a pit with a ramp out of its far side; g2's a deck to drive under
    // and, on past the middle, a long ramp up to a storey at its top.
    for (ETCS::Entity* b : battle)
    {
        verb(b, "KartBattle.AddMap", "course");
        verb(b, "KartBattle.AddSpring", "0, -18");
        verb(b, "KartBattle.AddDeck", "0, -5.25, 6, 14.5, 2.5");
        verb(b, "KartBattle.AddPit", "0, 15, 8, 14, 2.5");
        verb(b, "KartBattle.AddRamp", "0, 11, 180, 4, 6, 0, 0, 2.5");
        verb(b, "KartBattle.AddDeck", "12, 0, 8, 6, 3.5");
        verb(b, "KartBattle.AddRamp", "-16, 0, 270, 4, 12, 0, 0, 4.5");
        verb(b, "KartBattle.AddDeck", "-34, 0, 24, 6, 4.5");
        verb(b, "KartBattle.AddSpawn", "0, -24");
        verb(b, "KartBattle.AddSpawn", "0, 24");
        verb(b, "KartBattle.AddSpawn", "24, 0");
        verb(b, "KartBattle.AddPickup", "0, -8.5");
        // An eighth, the edges: three lanes. South, lava across the host's;
        // west from the east, a spring at the far wall; south from the north,
        // a bank up to a jump with a pad on its slope.
        verb(b, "KartBattle.AddMap", "edges");
        verb(b, "KartBattle.AddLava", "0, -35, 10, 10, 3");
        verb(b, "KartBattle.AddSpring", "-85, 0");
        verb(b, "KartBattle.AddRamp", "0, 38, 180, 8, 12, 0, 0, 2");
        verb(b, "KartBattle.AddPad", "0, 38, 180");
        verb(b, "KartBattle.AddSpawn", "0, -50");
        verb(b, "KartBattle.AddSpawn", "30, 0");
        verb(b, "KartBattle.AddSpawn", "0, 50");
        // A ninth, cover: g1 behind a block, straight down the host's lane.
        verb(b, "KartBattle.AddMap", "cover");
        verb(b, "KartBattle.AddBlock", "0, 17, 6, 1, 3, 0");
        verb(b, "KartBattle.AddSpawn", "0, -20");
        verb(b, "KartBattle.AddSpawn", "0, 19");
        verb(b, "KartBattle.AddSpawn", "25, -10");
        // A tenth, the range: open ground. g1 up the host's lane and twelve
        // to its side; g2 three and a half behind the host.
        verb(b, "KartBattle.AddMap", "range");
        verb(b, "KartBattle.AddSpawn", "0, -20");
        verb(b, "KartBattle.AddSpawn", "12, 20");
        verb(b, "KartBattle.AddSpawn", "0, -24");
    }
    {
        const std::string faults = verb(host, "KartBattle.Faults");
        std::printf("        (%s)\n", faults.c_str());
        // (This test's own maps are bare ground by design, and are told so too -- after what is wrong.)
        check(faults.compare(0, 38, "course: pit 0 15: a spawn at its edge\n") == 0, "...and a map with a spawn put at a pit's edge is told so");
    }
    // An eleventh, bends: an arc too steep at its inside edge for a kart to
    // climb, and a deck turned over a ramp, with no room left under it.
    for (ETCS::Entity* b : battle)
    {
        verb(b, "KartBattle.AddMap", "bends");
        verb(b, "KartBattle.AddArc", "0, 0, 10, 0, 1, 8, 4, 0, 0, 2.4");
        verb(b, "KartBattle.AddRamp", "40, 0, 45, 4, 20, 0, 0, 2.5");
        verb(b, "KartBattle.AddDeckAt", "40, 0, 45, 4, 20, 3");
        verb(b, "KartBattle.AddSpawn", "0, -50");
        verb(b, "KartBattle.AddSpawn", "30, -50");
        verb(b, "KartBattle.AddSpawn", "0, 50");
    }
    {
        const std::string faults = verb(host, "KartBattle.Faults");
        check(faults.find("bends: arc 0 0: its inside edge too steep to climb") != std::string::npos
              && faults.find("bends: deck 40 0: no room to drive under it") != std::string::npos,
              "...and an arc too steep inside, or a turned deck over a ramp, is told so");
    }

    // A seat and a mirror per guest, wired as the page wires them -- with the
    // page's own stream scripts.
    verb(host, "KartBattle.Host", "host 0 t1");
    verb(g1, "KartBattle.Join", "g1 host");
    {
        ETCS::ExecutionContext wire(&root, &ctx);
        auto bind = [&](const char* n, ETCS::Entity* e) { wire.names[n] = ETCS::NameBinding{ e->getRID(), "KartProvider", "KartBattle" }; };
        bind("host", host); bind("g1", g1);
        std::istringstream lines(
            "detach NetworkProvider/scripts/session_follow.etcs src=host dst=g1\n"
            "detach NetworkProvider/scripts/session_emit.etcs src=g1 dst=host\n");
        const std::string origin = std::string(ETCS_ACE_ROOT) + "/modules/kart_wiring.etcs";
        check(ETCS::run_script(lines, origin, wire), "the streams are wired with the page's scripts");
    }
    // The host's clock, by hand, and the guests let catch up with it.
    auto settle = [&]() {
        const uint64_t t = status(host).tick;
        auto behind = [&](ETCS::Entity* g) { const Status s = status(g); return s.mode == "guest" && s.tick != t; };
        for (int i = 0; i < 400 && (behind(g1) || behind(g2)); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    auto run = [&](uint32_t ticks) { verb(host, "KartBattle.Run", std::to_string(ticks)); settle(); };
    // A round waits for every seated driver to lock in: each says so, and the
    // host starts once its trace has them all.
    auto start = [&]() {
        for (ETCS::Entity* b : battle) verb(b, "KartBattle.LockIn", "1");
        std::string r = verb(host, "KartBattle.StartRound");
        for (int i = 0; i < 1000 && r != "ok"; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); r = verb(host, "KartBattle.StartRound"); }
        return r;
    };
    auto key = [&](ETCS::Entity* b, int k, bool down) {
        verb(b, "KartBattle.Key", std::to_string(k) + " " + (down ? "1" : "0"));
    };
    // A guest's key reaches the host on its seat: wait for it to be taken
    // (the host's trace grows) before the clock runs on.
    auto proposed = [&](ETCS::Entity* b, int k, bool down) {
        const uint64_t before = status(host).lines;
        key(b, k, down);
        for (int i = 0; i < 400 && status(host).lines == before; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    auto fire = [&](ETCS::Entity* b) { key(b, ' ', true); run(1); key(b, ' ', false); run(1); };

    // -- 1. the lobby ---------------------------------------------------------
    std::cout << "\n-- 1. the lobby --\n";
    // g1 sits before g2 does, so who sits where is the trace's order: the host first, then g1 furthest from it, then g2 furthest from both.
    for (int i = 0; i < 200 && where(host, "g1").slot < 0; ++i) run(2);
    verb(g2, "KartBattle.Join", "g2 host");
    {
        ETCS::ExecutionContext wire(&root, &ctx);
        wire.names["g2"]   = ETCS::NameBinding{ g2->getRID(), "KartProvider", "KartBattle" };
        wire.names["host"] = ETCS::NameBinding{ host->getRID(), "KartProvider", "KartBattle" };
        // After the Join, as session_join.etcs has it: a Join starts the battle
        // afresh, so a trace followed before it would be lines lost. Late, it
        // mirrors the host's from its first line.
        std::istringstream lines("detach NetworkProvider/scripts/session_follow.etcs src=host dst=g2\n"
                                 "detach NetworkProvider/scripts/session_emit.etcs src=g2 dst=host\n");
        ETCS::run_script(lines, std::string(ETCS_ACE_ROOT) + "/modules/kart_wiring.etcs", wire);
    }
    for (int i = 0; i < 200 && where(host, "g2").slot < 0; ++i) run(2);
    const Kart h0 = where(host, "host"), a0 = where(host, "g1"), b0 = where(host, "g2");
    std::printf("        (host %.1f,%.1f  g1 %.1f,%.1f  g2 %.1f,%.1f)\n", h0.x, h0.z, a0.x, a0.z, b0.x, b0.z);
    check(h0.slot == 0 && a0.slot == 1 && b0.slot == 2, "three drivers, each in a kart of their own");
    check(near(h0.x, 0, 0.01f) && near(h0.z, -61.5f, 0.01f) && near(h0.hz, 1, 1e-3f) && h0.speed == 0.0f && near(h0.y, 0.25f, 1e-3f),
          "the host at the bowl's first spawn, on the ground, facing the middle, still");
    check(near(a0.z, 61.5f, 0.01f) && near(a0.hz, -1, 1e-3f), "g1 at the spawn furthest from the host, facing it across the middle");
    check(near(b0.x, 61.5f, 0.01f) && near(b0.z, 0, 0.01f) && near(b0.hx, -1, 1e-3f), "g2 at the spawn furthest from both");
    check(h0.hp == 100 && a0.weapon == "-" && status(g2).phase == "lobby", "whole and unarmed, in the lobby");
    check(verb(host, "KartBattle.Here") == "g1\ng2\n" && verb(g2, "KartBattle.Here") == "host\n",
          "the host has both seats here, a guest the host whose trace it follows");
    key(host, 'W', true);
    run(50);
    key(host, 'W', false);
    check(near(where(host, "host").z, -61.5f, 1e-3f), "in the lobby the karts are parked: W moves nobody");
    check(verb(g1, "KartBattle.Configure", "pillars 60") == "refused" && verb(g1, "KartBattle.StartRound") == "refused",
          "a guest sets nothing: the map, the length and the round are the host's");

    // A look is each driver's own, and the lobby's: a colour of the palette
    // and a name, a line of the trace -- so every runtime shows it.
    {
        auto livery = [&](ETCS::Entity* b) { std::string c, n; std::istringstream in(verb(b, "KartBattle.Livery")); in >> c >> n; return c + " " + n; };
        check(livery(g1) == "1 g1" && livery(host) == "0 host", "a driver's look to begin with: its kart's own colour, its seat's label");
        // A guest's line reaches the host on its seat: wait until it is taken.
        auto said = [&](ETCS::Entity* b, const char* action, const char* args) {
            const uint64_t before = status(host).lines;
            const std::string answer = verb(b, action, args);
            for (int i = 0; i < 400 && answer == "ok" && status(host).lines == before; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return answer;
        };
        // One after the other, before either is back: neither undoes the other.
        check(verb(g1, "KartBattle.Recolour", "8") == "ok" && said(g1, "KartBattle.Rename", "zed") == "ok", "g1 takes the ninth colour and the name zed");
        for (int i = 0; i < 100 && livery(g1) != "8 zed"; ++i) run(2);
        check(livery(g1) == "8 zed", "...and has them");
        check(score(host, "zed").hp == 100 && score(g2, "zed").hp == 100 && score(host, "g1").hp == -1, "...on every runtime: the board has zed, not g1");
        check(where(host, "zed").slot == 1 && where(host, "g1").slot == 1, "a driver is still found by its seat, and by its name too");
        check(verb(g2, "KartBattle.Rename", "zed") == "refused" && verb(g2, "KartBattle.Rename", "host") == "refused",
              "a name is one driver's: not another's, nor another's seat");
        check(verb(g2, "KartBattle.Rename", "no!") == "refused" && verb(g2, "KartBattle.Recolour", "10") == "refused",
              "...and letters, digits, - and _ only; a colour is one of the ten");
        check(said(g1, "KartBattle.Rename", "") == "ok", "no name is the seat's label again");
        for (int i = 0; i < 100 && livery(g1) != "8 g1"; ++i) run(2);
        check(livery(g1) == "8 g1" && score(g2, "g1").hp == 100, "...everywhere, the colour kept");
    }

    // The settings: this runtime's alone. As made the marks keep eighty units
    // of road; the checks below are of forty, so the host's are set to that.
    check(verb(host, "KartBattle.Settings") == "invert 0 marks 80 look 1.000", "a runtime's settings as made: the wheel as it is, eighty units of marks, the look at its pace");
    check(verb(host, "KartBattle.Setting", "marks 40") == "ok" && verb(host, "KartBattle.Setting", "marks lots") == "refused"
          && verb(host, "KartBattle.Settings") == "invert 0 marks 40 look 1.000" && verb(g1, "KartBattle.Settings") == "invert 0 marks 80 look 1.000",
          "...set on one runtime, and on that one alone");
    // -- 2. a round, abandoned ------------------------------------------------
    std::cout << "\n-- 2. a round, abandoned --\n";
    check(start() == "ok", "the host starts a round on the bowl");
    run(2);
    check(status(g1).phase == "playing" && status(g2).phase == "playing", "...on every runtime");
    check(verb(host, "KartBattle.Configure", "crossroads 60") == "refused" && status(host).map == "bowl",
          "its settings are locked: no config while it runs");
    key(host, 'W', true);
    run(55);
    const Kart h1 = where(host, "host");
    std::printf("        (after 1.1 s: z %.2f, x %.3f, %.1f u/s, y %.2f %s)\n", h1.z, h1.x, h1.speed, h1.y, h1.air.c_str());
    check(h1.z - h0.z > 1.5f && h1.speed > 3.5f && h1.speed < 4.8f && near(h1.x, 0, 0.05f), "W: toward the middle, in a straight line, slow off the line");
    run(240);
    key(host, 'W', false);
    const Kart h2 = where(host, "host");
    std::printf("        (stopped at z %.2f, y %.2f)\n", h2.z, h2.y);
    check(near(h2.y, -2.25f, 0.01f), "...over the ring road's edge and down the ramp into the bowl's low ground");
    check(h2.z > -25.8f && h2.z < -24.8f, "the bumper in its way there stops it: solid meets solid");
    {
        // The wheel, standing still: A puts it at half the lock at once -- to
        // the left, the wheel's minus -- and held, it eases on to twice the lock.
        auto left = [&]() { return -where(host, "g2").wheel; };
        proposed(g2, 'A', true);
        run(10);
        const float w10 = left();
        run(10);
        const float w20 = left();
        run(30);
        const float w50 = left();
        run(60);
        const float w110 = left();
        proposed(g2, 'A', false);
        std::printf("        (wheel held: %.3f at 0.2 s, %.3f at 0.4 s, %.3f at 1 s, %.3f at 2.2 s)\n", w10, w20, w50, w110);
        check(w10 >= 0.29f && w10 < 0.34f, "A held: the wheel is at half the lock at once, and easing on");
        check(w20 > w10 && w50 > w20 && w50 < 0.80f && near(w110, 0.9f, 0.002f),
              "...and eases on, over a second and a half, to twice the lock");
        run(25);
        const float back = left();
        proposed(g2, 'A', true);
        run(4);
        proposed(g2, 'A', false);
        const float tap = left();
        run(25);
        std::printf("        (let go: %.3f after 0.5 s; a tap of 4 ticks: %.3f; then %.3f)\n", back, tap, left());
        check(near(back, 0, 1e-3f) && tap >= 0.225f && tap < 0.27f && near(left(), 0, 1e-3f),
              "let go, it comes back to the middle; a tap is half the lock, and little more");
        // The keys swapped, on g2's runtime alone: A is the right now, and
        // swapped back with A still held it is the left again.
        verb(g2, "KartBattle.SetInvert", "1");
        proposed(g2, 'A', true);
        run(10);
        const float swapped = left();
        const uint64_t before = status(host).lines;
        verb(g2, "KartBattle.SetInvert", "0");
        for (int i = 0; i < 400 && status(host).lines == before; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        run(30);
        const float unswapped = left();
        proposed(g2, 'A', false);
        run(40);
        std::printf("        (inverted, A turned it %.3f; put back with A held, %.3f)\n", swapped, unswapped);
        check(swapped < -0.01f && unswapped > 0.01f && near(left(), 0, 1e-3f),
              "inverted steering swaps the wheel's keys, for the runtime that set it, at once");
    }
    proposed(g2, 'A', true);
    run(50);
    const Kart b1 = where(host, "g2");
    proposed(g2, 'A', false);
    check(near(b1.hx, -1, 1e-3f) && b1.speed == 0.0f, "A standing still: the wheels turn, the kart does not");
    {
        // The wheels seen: g2's four, its kart's members (found as the drawn
        // things round the middle of its frame), each axle (its cylinder's
        // axis) across the kart -- the front two turned left by the lock, the
        // rear two square.
        Causal_* wc = static_cast<Causal_*>(worlds[0]->getInterfacePointer(ETCS::Buffer("Causal")));
        std::vector<Causal_*> karts, by;
        wc->Near(Fixed::From(b1.x), Fixed::From(0.25), Fixed::From(b1.z), Fixed::From(0.1), karts);
        Causal_* body = nullptr;
        for (Causal_* c : karts) if (c->Solid().shape == CausalSolid::Sphere) body = c;
        if (body) body->Near(Fixed::Zero(), Fixed::Zero(), Fixed::Zero(), Fixed::From(1.4), by);
        float front = 0, rear = 0; int nf = 0, nr = 0;
        for (Causal_* c : by)
        {
            std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
            const OrderVector& r = c->Order4();
            // A wheel by its size: a reach of |(0.23, 0.12, 0.23)|. The kart,
            // the ground and the bar over it are all other sizes.
            if (c->Solid().shape != CausalSolid::Off || std::fabs(r.radius.ToFloat() - 0.347f) > 0.01f) continue;
            Fixed ax = Fixed::Zero(), ay = Fixed::One(), az = Fixed::Zero();
            r.RotateVector(ax, ay, az);
            // How far the axle is off the kart's own across (hz, -hx), either end.
            const float dot = std::fabs(ax.ToFloat() * b1.hz - az.ToFloat() * b1.hx);
            const float a = std::acos(dot > 1.0f ? 1.0f : dot);
            const float along = r.x.ToFloat() * b1.hx + r.z.ToFloat() * b1.hz;   // in the kart's frame
            if (along > 0) { front += a; ++nf; } else { rear += a; ++nr; }
        }
        std::printf("        (axles off square: front %.3f rad, rear %.3f rad, %d+%d wheels)\n", nf ? front / nf : -1.f, nr ? rear / nr : -1.f, nf, nr);
        check(nf == 2 && nr == 2 && std::fabs(front / nf - std::fabs(b1.wheel)) < 0.02f && std::fabs(b1.wheel) > 0.6f && rear / nr < 0.01f,
              "...and the front wheels, the kart's own members, are seen turned as far as the wheel is, the rear ones square");
    }
    verb(host, "KartBattle.Abandon");
    run(2);
    check(status(g1).phase == "lobby" && near(where(host, "host").z, -61.5f, 0.01f),
          "the host abandons the round: back in the lobby on every runtime, everyone respawned, parked");

    // -- 3. the settings -----------------------------------------------------
    std::cout << "\n-- 3. the settings --\n";
    check(verb(host, "KartBattle.Configure", "crossroads 30") == "ok", "in the lobby the host sets crossroads, thirty seconds");
    run(2);
    const Status s3 = status(g2);
    const Kart h3 = where(host, "host"), a3 = where(host, "g1"), b3 = where(host, "g2");
    std::printf("        (host %.1f,%.1f  g1 %.1f,%.1f  g2 %.1f,%.1f)\n", h3.x, h3.z, a3.x, a3.z, b3.x, b3.z);
    check(s3.map == "crossroads" && s3.secs == 30 && s3.phase == "lobby", "...and the map is out at once, on every runtime");
    check(near(h3.z, -81, 0.01f) && near(a3.z, 81, 0.01f) && near(b3.x, -81, 0.01f) && near(a3.hz, -1, 1e-3f),
          "...with everyone respawned on it: the host and g1 face to face down a road");
    {
        // The bowl's bumper the host stopped at is gone from where it stood.
        Causal_* wc = static_cast<Causal_*>(worlds[1]->getInterfacePointer(ETCS::Buffer("Causal")));
        std::vector<Causal_*> by;
        wc->Near(Fixed::Zero(), Fixed::From(-2.1), Fixed::From(-23.1), Fixed::From(0.5), by);
        bool bump = false;
        // A bumper by its size: a reach of |(4, 0.4, 1.2)|.
        for (Causal_* c : by)
            if (c->Solid().shape == CausalSolid::Box && std::fabs(c->Order4().radius.ToFloat() - 4.195f) < 0.01f) bump = true;
        check(!bump, "...and the last map's blocks are out of the way (here on g1's world)");
    }

    // -- 4. a round, abandoned with a score, then one completed ---------------
    std::cout << "\n-- 4. a round, abandoned with a score, then one completed --\n";
    start();
    run(2);
    verb(host, "KartBattle.Give", "host sniper");
    fire(host);
    const int counted = score(g1, "host").elims;
    {
        // Up the road, braked to a stop, on up the road again, then D held at
        // speed: the marks the brake leaves, kept while it stands and wiped as
        // it goes on; the lock smaller the faster it goes; and a drift.
        key(host, 'W', true);
        run(100);
        const Kart k0 = where(host, "host");
        const Tyres c0 = tyres(host, "host");
        std::printf("        (straight at %.1f u/s after 2 s, %.1f along: sliding %.3f, %d mark segments)\n",
                    k0.speed, k0.z + 81.0f, k0.slide, c0.shown());
        check(k0.speed > 9.0f && k0.speed < 11.0f && std::fabs(k0.slide) < 0.01f && c0.shown() == 0,
              "speed builds gradually; in a straight line the tyres roll: no slide, no marks");
        key(host, 'W', false);
        key(host, 'S', true);
        for (int t = 0; t < 120 && where(host, "host").speed > 0.05f; ++t) run(1);
        key(host, 'S', false);
        const Tyres b0 = tyres(host, "host");
        run(50);
        const Tyres b1 = tyres(host, "host");
        std::printf("        (braked: ropes %.2f %.2f %.2f %.2f, marks %d %d %d %d, darkest %.2f %.2f %.2f %.2f)\n",
                    b0.t[0].rope, b0.t[1].rope, b0.t[2].rope, b0.t[3].rope, b0.t[0].shown, b0.t[1].shown, b0.t[2].shown,
                    b0.t[3].shown, b0.t[0].darkest, b0.t[1].darkest, b0.t[2].darkest, b0.t[3].darkest);
        bool four = true;
        for (const Tyre& t : b0.t) four = four && t.shown >= 1 && t.darkest >= 0.75f;
        check(four, "braked to a stop: every tyre leaves a dark mark");
        check(b1.shown() == b0.shown(), "...which stays while the kart stands");
        // Forty units of road wipe it: back down the road and let roll to a
        // stop (no brake, so no new mark), then up it again.
        const Kart kb = where(host, "host");
        key(host, 'S', true);
        run(60);
        key(host, 'S', false);
        run(560);
        const Kart kr = where(host, "host");
        const Tyres r0 = tyres(host, "host");
        key(host, 'W', true);
        run(255);
        const Tyres w0 = tyres(host, "host");
        const Kart k2 = where(host, "host");
        std::printf("        (back %.1f units: %d mark segments; on %.1f more: %d, ropes %.2f)\n",
                    kb.z - kr.z, r0.shown(), k2.z - kr.z, w0.shown(), w0.t[0].rope);
        check(r0.shown() >= b0.shown() && r0.shown() <= b0.shown() + 4 && kr.speed == 0.0f,
              "...and while the road since is short of the rope's forty units (the run open at the stop closed as it moves off)");
        check(w0.shown() == 0 && w0.t[0].rope >= 40.0f && w0.t[0].rope < 44.5f,
              "...and is wiped from the far end as it goes on, either way: the rope keeps its length");
        key(host, 'D', true);
        run(2);
        const Kart kp = where(host, "host");
        run(33);
        const Kart k1 = where(host, "host");
        const Tyres c1 = tyres(host, "host");
        key(host, 'D', false);
        key(host, 'W', false);
        const float lock = 0.45f / (1.0f + kp.speed / 10.0f);
        const float turned = std::acos(std::max(-1.0f, std::min(1.0f, k2.hx * k1.hx + k2.hz * k1.hz)));
        std::printf("        (at %.1f u/s the wheel is %.3f at once, half the lock there %.3f; held 0.7 s it is %.3f, turned %.2f rad, sliding %.2f u/s)\n",
                    kp.speed, kp.wheel, lock * 0.5f, k1.wheel, turned, k1.slide);
        std::printf("        (tyres fr fl rr rl: heat %.1f %.1f %.1f %.1f, marks %d %d %d %d, darkest %.2f %.2f %.2f %.2f)\n",
                    c1.t[0].heat, c1.t[1].heat, c1.t[2].heat, c1.t[3].heat, c1.t[0].shown, c1.t[1].shown, c1.t[2].shown,
                    c1.t[3].shown, c1.t[0].darkest, c1.t[1].darkest, c1.t[2].darkest, c1.t[3].darkest);
        check(kp.speed > 12.0f && kp.wheel > lock * 0.5f - 0.005f && kp.wheel < lock * 0.6f,
              "at speed D puts the wheel at once at half the smaller lock for that speed");
        check(k1.wheel > kp.wheel * 1.2f && k1.wheel < 0.9f && turned > 0.6f && turned < 2.2f,
              "...and held, it eases on past it: an arc that tightens");
        check(k1.hx > 0.3f && k1.slide < -0.5f,
              "...to the right, and more than the tyres can hold: the kart slides out of the turn -- a drift");
        std::printf("        (into the drift at %.1f u/s, %.1f in it)\n", k2.speed, k1.speed);
        check(k1.speed > 0.85f * k2.speed, "...which keeps nearly all its speed");
        check(c1.shown() > 0 && c1.t[0].heat + c1.t[1].heat + c1.t[2].heat + c1.t[3].heat > 0.0f,
              "...its tyres hot with the slide, and marking the road");
        run(50);
    }
    verb(host, "KartBattle.Abandon");
    run(2);
    std::printf("        (an elimination counted: %d; after the abandon: %d)\n", counted, score(g2, "host").elims);
    check(counted == 1 && status(g2).phase == "lobby" && score(g2, "host").elims == 0 && score(g2, "g1").deaths == 0,
          "an abandoned round's elimination is discarded with it, on every runtime");
    check(start() == "ok", "the host starts the round again");
    run(2);
    const Status s4 = status(g1);
    check(s4.phase == "playing" && s4.left == 30, "...on every runtime: playing, thirty seconds left");

    verb(host, "KartBattle.Give", "host sniper");
    fire(host);
    const Score e1 = score(g2, "host"), d1 = score(g2, "g1");
    std::printf("        (host %d elim(s), g1 %d death(s), %s)\n", e1.elims, d1.deaths, d1.state.c_str());
    check(e1.elims == 1 && d1.deaths == 1 && d1.state == "out" && d1.hp == 0,
          "the sniper dead on down the road: a whole kart, one elimination, one death");
    check(score(g2, "host").weapon == "-", "...and the sniper is spent with its one shot");
    run(149);
    const Kart a5 = where(host, "g1");
    check(a5.hp == 100 && near(a5.z, 81, 0.01f) && score(host, "g1").state == "alive",
          "three seconds later g1 is back, whole, at the spawn furthest from everyone");

    verb(host, "KartBattle.Give", "host rocket");
    fire(host);
    check(where(host, "host").weapon == "rocket" && where(host, "host").ammo == 1, "a rocket fired, one left");
    for (int i = 0; i < 300 && where(host, "g1").hp == 100; ++i) run(2);   // 162 units at 30 u/s
    const int rocket = 100 - where(host, "g1").hp;
    std::printf("        (the rocket did %d)\n", rocket);
    check(rocket >= 37 && rocket <= 40, "it bursts on the kart it meets: up to 40 at the burst's centre");

    verb(host, "KartBattle.Give", "host fuse");
    fire(host);
    run(130);
    check(where(host, "host").hp == 100, "a fuse burns out on the kart that lit it, and does it no harm");

    // g2 along the cross street: over the pad on it, which pushes it the way
    // it points, then a box.
    proposed(g2, 'W', true);
    std::string got = "-";
    float gain = 0.0f, last = 0.0f, padX = 0.0f, fastest = 0.0f;
    for (int i = 0; i < 200 && got == "-"; ++i)
    {
        run(2);
        const Kart k = where(host, "g2");
        if (k.speed - last > gain) { gain = k.speed - last; padX = k.x; }
        last = k.speed; fastest = std::max(fastest, k.speed);
        got = k.weapon;
    }
    proposed(g2, 'W', false);
    std::printf("        (g2 over the pad: %.1f u/s gained in two ticks at x %.1f, up to %.1f u/s)\n", gain, padX, fastest);
    check(gain > 1.2f && padX > -66.5f && padX < -60.0f && fastest > 14.0f,
          "a pad pushes a kart that drives over it the way it points: more than any pedal");
    std::printf("        (g2 drove over a box and got a %s)\n", got.c_str());
    check(got != "-" && where(g2, "g2").weapon == got, "a power-up hands over a weapon off the trace's dice, the same everywhere");

    // The rest of the arms, each fired once (and the battle, below, still one).
    verb(host, "KartBattle.Give", "host minigun");
    key(host, ' ', true);
    run(26);
    key(host, ' ', false);
    run(1);
    check(where(host, "host").ammo == 24, "the minigun fires while Space is held: six rounds in half a second");
    verb(host, "KartBattle.Give", "host shotgun");
    key(host, ' ', true);
    key(host, ' ', false);
    run(1);
    check(where(host, "host").ammo == 1, "a tap whose press and release fall between two ticks still fires");
    for (const char* w : { "bomb", "mine" }) { verb(host, "KartBattle.Give", std::string("host ") + w); fire(host); }
    run(60);
    verb(host, "KartBattle.Give", "host boost");
    key(host, 'W', true);
    fire(host);
    run(65);
    const float boosted = where(host, "host").speed;
    key(host, 'W', false);
    std::printf("        (boosted to %.1f)\n", boosted);
    check(boosted > 22.0f, "a boost takes a kart past its own top speed");

    run(static_cast<uint32_t>(status(host).left) * 50 + 2);
    const Status s6 = status(g1);
    check(s6.phase == "results" && score(g1, "host").elims == 1, "the clock runs out into the results, the scores standing");
    run(302);
    check(status(g2).phase == "lobby" && score(g2, "host").elims == 1,
          "...then the lobby, which keeps the round's scores (an abandoned one's it did not)");
    start();
    run(2);
    const Score e7 = score(g1, "host"), d7 = score(g1, "g1");
    check(e7.elims == 0 && d7.deaths == 0 && status(g1).phase == "playing", "the next round starts the scores afresh");
    verb(host, "KartBattle.Abandon");
    run(2);

    // -- 5. the ground ---------------------------------------------------------
    std::cout << "\n-- 5. the ground --\n";
    check(verb(host, "KartBattle.Configure", "skatepark 60") == "ok" && start() == "ok",
          "a round on the skatepark: two jumps down the middle");
    run(2);
    const Kart h5 = where(host, "host");
    check(near(h5.z, -66, 0.01f) && near(h5.hz, 1, 1e-3f), "the host at the foot of the near jump, facing it");
    {
        // W down; watched a tick at a time: up the ramp at the ramp's height,
        // off the lip, through the air, down again beyond it.
        key(host, 'W', true);
        float onRampErr = -1.0f, topY = 0.0f, landZ = 0.0f, liftZ = 0.0f;
        bool flew = false, landed = false;
        for (int t = 0; t < 400 && !landed; ++t)
        {
            run(1);
            const Kart k = where(host, "host");
            if (k.air == "ground" && k.z > -46.0f && k.z < -38.0f && onRampErr < 0.0f)
                onRampErr = std::fabs(k.y - (0.25f + 3.0f * (k.z + 48.0f) / 12.0f));
            if (k.air == "air" && !flew) { flew = true; liftZ = k.z; }
            if (flew) topY = std::max(topY, k.y);
            if (flew && k.air == "ground") { landed = true; landZ = k.z; }
        }
        key(host, 'W', false);
        key(host, 'S', true);
        for (int t = 0; t < 120 && where(host, "host").speed > 0.05f; ++t) run(1);
        key(host, 'S', false);
        std::printf("        (on the ramp %.3f off its height; off the lip at z %.2f, up to y %.2f, down at z %.2f)\n",
                    onRampErr, liftZ, topY, landZ);
        // Braked to a stop, then backed off over its own marks: what was laid
        // stays laid -- the way back is new road at the rope's front.
        run(50);
        const Tyres m0 = tyres(host, "host");
        key(host, 'S', true);
        run(80);
        key(host, 'S', false);
        run(30);
        const Tyres m2 = tyres(host, "host");
        std::printf("        (braked: marks %d, darkest %.2f; backed off to z %.1f: marks %d, darkest %.2f, ropes %.2f)\n",
                    m0.shown(), m0.t[0].darkest, where(host, "host").z, m2.shown(), m2.t[0].darkest, m2.t[0].rope);
        // (The run still open when it stopped -- the last, lightest of the
        // braking -- is closed as it moves off: at most one more a tyre.)
        bool kept = m0.shown() >= 4 && m2.shown() >= m0.shown() && m2.shown() <= m0.shown() + 4;
        for (int t = 0; t < 4; ++t) kept = kept && m2.t[t].darkest == m0.t[t].darkest && m2.t[t].rope < 44.5f;
        check(kept, "backing off over its own marks leaves them as they were laid, the rope its length");
        check(onRampErr >= 0.0f && onRampErr < 0.03f, "up the ramp, the kart rides at the ramp's height");
        check(flew && liftZ > -36.6f && liftZ < -35.4f && topY > 3.3f, "...leaves the ground at the lip and climbs higher still");
        check(landed && landZ > -34.0f, "...and lands beyond it");
    }
    {
        // g1 down the middle the other way, at the far jump's lip from below.
        proposed(g1, 'W', true);
        run(200);
        const Kart k = where(host, "g1");
        proposed(g1, 'W', false);
        std::printf("        (g1 stopped at z %.2f, y %.2f)\n", k.z, k.y);
        check(k.z > 48.4f && k.z < 49.6f && near(k.y, 0.25f, 0.01f) && k.speed < 0.5f,
              "a lip met from the drop side is a wall: g1 stops at it, on the ground");
    }
    verb(host, "KartBattle.Abandon");
    run(2);

    // -- 6. a map from a seed --------------------------------------------------
    std::cout << "\n-- 6. a map from a seed --\n";
    check(verb(host, "KartBattle.Configure", "random 60") == "ok", "the host picks the map made from a seed");
    run(2);
    const std::string l1 = verb(host, "KartBattle.Layout");
    std::printf("        (%s)\n", l1.c_str());
    check(l1 == verb(g1, "KartBattle.Layout") && l1 == verb(g2, "KartBattle.Layout") && status(g1).layout != 0,
          "...the same layout on every runtime, its seed in the trace");
    check(verb(host, "KartBattle.Reroll") == "ok", "a new seed");
    run(2);
    const std::string l2 = verb(host, "KartBattle.Layout");
    std::printf("        (%s)\n", l2.c_str());
    check(l2 != l1 && l2 == verb(g1, "KartBattle.Layout") && l2 == verb(g2, "KartBattle.Layout"),
          "...a new layout, still the same everywhere");
    {
        std::istringstream in(l2);
        std::string name; uint64_t seed; int blocks, ramps, hills, spawns, pickups, pads, springs, decks, pits;
        in >> name >> seed >> blocks >> ramps >> hills >> spawns >> pickups >> pads >> springs >> decks >> pits;
        bool grounded = true;
        for (const char* who : { "host", "g1", "g2" })
        {
            const Kart k = where(host, who);
            grounded = grounded && k.air == "ground" && (near(k.y, 0.25f, 0.01f) || near(k.y, -2.25f, 0.01f));   // the high ground, or the low
        }
        check(spawns == 8 && pickups >= 1 && pads >= 1 && springs >= 2 && decks >= 1 && pits <= 1
              && blocks + ramps + hills >= 3 && grounded,
              "...with things on it -- pads, springs and a deck too -- eight spawns, and every kart on level ground at one");
    }
    check(start() == "ok", "a round on it");
    proposed(g1, 'W', true);
    proposed(g2, 'D', true);
    proposed(g2, 'W', true);
    key(host, 'W', true);
    run(300);
    key(host, 'W', false);
    proposed(g1, 'W', false);
    proposed(g2, 'W', false);
    proposed(g2, 'D', false);

    // -- 7. the levels -------------------------------------------------------
    std::cout << "\n-- 7. the levels --\n";
    verb(host, "KartBattle.Abandon");
    run(2);
    check(verb(host, "KartBattle.Configure", "course 60") == "ok" && start() == "ok",
          "a round on the course: a spring, a pit, a deck to drive under, a deck to land on and a storey up a ramp");
    run(2);
    {
        const std::string lc = verb(host, "KartBattle.Layout");
        const Kart c0 = where(host, "host"), c1 = where(host, "g1"), c2 = where(host, "g2");
        std::printf("        (%s; host %.0f,%.0f  g1 %.0f,%.0f  g2 %.0f,%.0f)\n", lc.c_str(), c0.x, c0.z, c1.x, c1.z, c2.x, c2.z);
        check(lc == verb(g1, "KartBattle.Layout") && lc == verb(g2, "KartBattle.Layout") && near(c0.z, -24, 0.01f)
              && near(c1.z, 24, 0.01f) && near(c2.x, 24, 0.01f), "...the same on every runtime, each kart at the head of its lane");
        // The eye, before anyone moves: g2's own, behind its kart and over
        // it; looked round to the right it swings to the kart's left side,
        // stays while the look is fresh, and then comes back -- slowly.
        struct View { float ex = 0, ey = 0, ez = 0, ax = 0, ay = 0, az = 0, yaw = 0, pitch = 0; };
        auto eye = [&](ETCS::Entity* b) { View v; std::istringstream in(verb(b, "KartBattle.Eye")); in >> v.ex >> v.ey >> v.ez >> v.ax >> v.ay >> v.az >> v.yaw >> v.pitch; return v; };
        auto frames = [&](ETCS::Entity* b, int n) { for (int i = 0; i < n; ++i) verb(b, "KartBattle.Frame", "100"); };
        {
            const View v0 = eye(g2);
            verb(g2, "KartBattle.Look", "90 0");
            const View v1 = eye(g2);
            frames(g2, 5);
            const View v2 = eye(g2);
            frames(g2, 17);
            const View v3 = eye(g2);
            frames(g2, 120);
            const View v4 = eye(g2);
            std::printf("        (g2's eye %.2f %.2f %.2f; looked right %.2f %.2f %.2f; the look %.0f after 0.5 s, %.0f after 2.2 s, %.0f after 14 s)\n",
                        v0.ex, v0.ey, v0.ez, v1.ex, v1.ey, v1.ez, v2.yaw, v3.yaw, v4.yaw);
            check(near(v0.ex, 31.5f, 0.05f) && near(v0.ey, 3.85f, 0.05f) && near(v0.ez, 0, 0.05f) && near(v0.ax, 20, 0.05f),
                  "the eye rides behind its kart and over it, looking past it");
            check(near(v1.ex, 24, 0.05f) && near(v1.ez, -7.5f, 0.05f) && near(v1.ey, 3.85f, 0.05f) && near(v1.az, 4, 0.05f),
                  "looked round to the right, it swings to the kart's left side and looks across it");
            check(near(v2.yaw, 90, 0.01f) && v3.yaw > 20.0f && v3.yaw < 50.0f && near(v4.yaw, 0, 0.01f) && near(v4.ex, 31.5f, 0.05f),
                  "...holds while the look is fresh, then drifts back behind the kart, slowly");
            verb(g2, "KartBattle.Key", "69 1");
            frames(g2, 5);
            verb(g2, "KartBattle.Key", "69 0");
            const View v5 = eye(g2);
            verb(g2, "KartBattle.Key", "81 1");
            frames(g2, 5);
            verb(g2, "KartBattle.Key", "81 0");
            const View v6 = eye(g2);
            std::printf("        (E held half a second: %.0f degrees; then Q as long: %.0f)\n", v5.yaw, v6.yaw);
            check(near(v5.yaw, 57.3f, 0.5f) && near(v6.yaw, 0, 0.5f) && status(host).phase == "playing",
                  "E looks right and Q left while held -- this runtime's alone, no line of the trace");
            verb(g2, "KartBattle.Look", "0 0");
        }
        // g2 down its lane, under the deck over it -- its eye under it too --
        // on across the middle, up the long ramp and onto the storey at its top.
        proposed(g2, 'W', true);
        float under = 0.0f, eye_under = 0.0f, eye_x = 0.0f, top = 0.0f; int below = 0; bool flew = false, up = false;
        for (int t = 0; t < 700 && !up; ++t)
        {
            run(1);
            const Kart k = where(host, "g2");
            if (k.x > 8.5f && k.x < 15.5f) { ++below; under = std::max(under, std::fabs(k.y - 0.25f)); if (k.air != "ground") under = 9.0f; }
            if (k.x > 9.0f && k.x < 11.0f) { const View v = eye(g2); eye_under = std::max(eye_under, v.ey); eye_x = v.ex; }
            if (k.x < -10.0f && k.x > -31.0f) { top = std::max(top, k.y); flew = flew || k.air != "ground"; }
            up = k.x < -30.9f && k.air == "ground";
        }
        proposed(g2, 'W', false);
        const Kart s2 = where(host, "g2");
        const View vs = eye(g2);
        std::printf("        (g2 under the deck for %d ticks, %.3f off the floor's height, its eye at y %.2f, x %.1f)\n", below, under, eye_under, eye_x);
        std::printf("        (up the ramp to y %.2f, off its top into the air: %d; on the storey at x %.1f, y %.2f, its eye at y %.2f)\n", top, flew, s2.x, s2.y, vs.ey);
        check(below > 10 && under < 0.01f, "a kart below a deck drives under it, on the floor");
        check(eye_under > 1.0f && eye_under < 3.25f && eye_x < 16.0f, "...and its eye comes in under the deck with it");
        check(up && top > 4.74f && near(s2.y, 4.75f, 0.01f),
              "a long ramp up to a storey's edge is a way up: driven up it and straight on, a kart is on the storey");
        check(vs.ey > 8.0f, "...and up there its eye is over it again");
        // g1 down its lane: into the pit, across its floor, up the ramp out.
        proposed(g1, 'W', true);
        float deepest = 0.0f, floorZ = 0.0f; bool fell = false, out = false;
        View pit_eye;
        for (int t = 0; t < 400 && !out; ++t)
        {
            run(1);
            const Kart k = where(host, "g1");
            if (k.air == "air" && k.z < 22.5f && k.z > 14.0f) fell = true;
            if (k.air == "ground" && k.y < deepest) { deepest = k.y; floorZ = k.z; pit_eye = eye(g1); }
            out = k.z < 7.0f && k.air == "ground" && near(k.y, 0.25f, 0.02f);
        }
        proposed(g1, 'W', false);
        proposed(g1, 'S', true);
        for (int t = 0; t < 120 && where(host, "g1").speed > 0.05f; ++t) run(1);
        proposed(g1, 'S', false);
        std::printf("        (g1 fell in: %d, down to y %.2f at z %.1f; out: %d, stopped at z %.1f)\n", fell, deepest, floorZ, out, where(host, "g1").z);
        std::printf("        (g1's eye as it landed: z %.2f, y %.2f)\n", pit_eye.ez, pit_eye.ey);
        check(fell && near(deepest, -2.25f, 0.01f) && floorZ > 14.0f, "a kart driven into a pit falls to its floor");
        check(pit_eye.ez < 22.0f && pit_eye.ez > floorZ && pit_eye.ey > -1.5f && pit_eye.ey < 1.35f,
              "...and its eye comes down into the pit with it, this side of the wall behind");
        check(out, "...and the ramp as high as the pit is deep is the way back up");
        // The host up its lane: onto the spring, up, and down on the deck.
        key(host, 'W', true);
        float peak = 0.0f, onZ = 0.0f, onY = 0.0f, upZ = 0.0f, upV = 0.0f, was = 0.0f; bool thrown = false, on = false;
        for (int t = 0; t < 300 && !on; ++t)
        {
            run(1);
            const Kart k = where(host, "host");
            if (k.air == "air" && !thrown) { thrown = true; upZ = k.z; upV = was; }
            was = k.speed;
            peak = std::max(peak, k.y);
            if (thrown && k.air == "ground") { on = true; onZ = k.z; onY = k.y; }
        }
        key(host, 'W', false);
        key(host, 'S', true);
        for (int t = 0; t < 120 && where(host, "host").speed > 0.05f; ++t) run(1);
        key(host, 'S', false);
        const Kart d0 = where(host, "host");
        // Up at 7.5 u/s standing and half the kart's speed more: as high as that goes under 10 u/s/s.
        const float kick = 7.5f + 0.5f * upV, high = 0.25f + kick * kick / 20.0f;
        std::printf("        (thrown up at z %.1f at %.1f u/s to y %.2f (%.2f for a kick of %.1f); down at z %.1f, y %.2f; stopped at z %.1f, y %.2f)\n",
                    upZ, upV, peak, high, kick, onZ, onY, d0.z, d0.y);
        check(thrown && upZ > -19.6f && upZ < -16.4f && std::fabs(peak - high) < 0.15f,
              "a spring throws a kart up: a hop standing, and half the kart's speed more");
        check(on && near(onY, 2.75f, 0.01f) && onZ > -12.5f && onZ < 2.0f && near(d0.y, 2.75f, 0.01f),
              "...and it comes down on the deck: a level plane in the air, and it stands on it");
        key(host, 'W', true);
        bool off = false;
        for (int t = 0; t < 200 && !off; ++t) { run(1); const Kart k = where(host, "host"); off = k.air == "ground" && k.y < 0.26f; }
        key(host, 'W', false);
        key(host, 'S', true);
        for (int t = 0; t < 120 && where(host, "host").speed > 0.05f; ++t) run(1);
        key(host, 'S', false);
        // At the pedal's pace from a stand on the deck it carries over the
        // floor and into the pit past it: down either way.
        std::printf("        (off the deck's edge: down at z %.1f, y %.2f)\n", where(host, "host").z, where(host, "host").y);
        check(off && where(host, "host").z > 2.0f, "...and off its edge it falls back down");
    }

    // -- 8. the edges ---------------------------------------------------------
    std::cout << "\n-- 8. the edges --\n";
    verb(host, "KartBattle.Abandon");
    run(2);
    check(verb(host, "KartBattle.Configure", "edges 60") == "ok" && start() == "ok",
          "a round on the edges: lava, a spring by the wall, a pad on a bank");
    run(2);
    {
        const Kart e0 = where(host, "host"), e1 = where(host, "g1"), e2 = where(host, "g2");
        check(near(e0.z, -50, 0.01f) && near(e1.z, 50, 0.01f) && near(e2.x, 30, 0.01f), "...each kart at the head of its lane");
        // The host down its lane into the lava.
        key(host, 'W', true);
        int ticks = 0;
        for (; ticks < 300 && score(host, "host").state == "alive"; ++ticks) run(1);
        key(host, 'W', false);
        const std::string lava = verb(host, "KartBattle.Note");
        std::printf("        (the host out after %d ticks: \"%s\")\n", ticks, lava.c_str());
        check(score(g1, "host").deaths == 1 && score(g1, "host").state == "out" && lava == "the lava took out host",
              "a kart driven into lava is out, on every runtime: the lava took it");
        // g2 the length of the arena at the spring by the far wall.
        proposed(g2, 'W', true);
        float peak = 0.0f, fast = 0.0f, lastX = 0.0f;
        for (ticks = 0; ticks < 600 && score(host, "g2").state == "alive"; ++ticks)
        {
            run(1);
            const Kart k = where(host, "g2");
            if (k.slot < 0 || k.hp == 0) break;
            peak = std::max(peak, k.y); lastX = k.x;
            if (k.x > -82.0f) fast = k.speed;
        }
        proposed(g2, 'W', false);
        const std::string drop = verb(host, "KartBattle.Note");
        std::printf("        (g2 at the spring at %.1f u/s, up to y %.2f, last seen at x %.1f: \"%s\")\n", fast, peak, lastX, drop.c_str());
        check(fast > 18.0f && peak > 7.0f, "a spring taken at speed throws a kart higher than the arena's wall");
        // Clear over the rim's mountains (sixteen up) at that speed: out there it
        // has no ground, and is out -- by the drop, or by its three seconds beyond the walls.
        check(lastX < -104.0f && score(g1, "g2").state == "out" && (drop == "the drop took out g2" || drop == "the mountains took out g2"),
              "...and over it and the mountains behind it: out there it is out");
        // g1 up the bank: the pad on its slope pushes it up it.
        proposed(g1, 'W', true);
        float gain = 0.0f, gainY = 0.0f, last = 0.0f;
        for (int t = 0; t < 80; ++t)
        {
            run(2);
            const Kart k = where(host, "g1");
            if (k.air == "ground" && k.y > 0.3f && k.speed - last > gain) { gain = k.speed - last; gainY = k.y; }
            last = k.speed;
            if (k.z < 30.0f) break;
        }
        proposed(g1, 'W', false);
        std::printf("        (g1 up the bank: %.2f u/s gained in two ticks at y %.2f)\n", gain, gainY);
        check(gain > 1.0f && gainY > 0.5f && gainY < 2.25f, "a pad on a bank's slope pushes a kart up it: more than any pedal");
    }

    // The fire goes round what is dense: a rocket into the block in front of
    // g1 bursts within its old reach of g1, and the block between keeps it off.
    verb(host, "KartBattle.Abandon");
    run(2);
    check(verb(host, "KartBattle.Configure", "cover 60") == "ok" && start() == "ok",
          "a round on cover: g1 behind a block down the host's lane");
    run(2);
    {
        const Kart c0 = where(host, "host"), c1 = where(host, "g1");
        verb(host, "KartBattle.Give", "host rocket");
        fire(host);
        run(120);
        const Kart c2 = where(host, "g1");
        // The rocket bursts on the block's face, about 16.3: g1's middle is
        // 2.7 from it, well in a rocket's reach of 4 -- the straight falloff
        // would have taken some 22 off it.
        std::printf("        (host at z %.1f, g1 at z %.1f behind a block at 16.5..17.5: after the rocket g1 has %d)\n", c0.z, c1.z, c2.hp);
        check(near(c0.z, -20, 0.01f) && near(c1.z, 19, 0.01f) && where(host, "host").ammo == 1 && c2.hp == 100,
              "a burst against a block does not reach the kart behind it: the fire goes round, and runs out");
        // "<tree> <turn> <bends> <steps> <cut> <cut unbent>": the block bent the tree.
        int tree = -1, turn = 0, bends = 0, steps = 0, cut = 0, straight = 0;
        std::istringstream lb(verb(host, "KartBattle.LastBurst"));
        lb >> tree >> turn >> bends >> steps >> cut >> straight;
        std::printf("        (the burst at the block: tree %d turned %d, %d bends; %d of %d steps cut, %d unbent)\n", tree, turn, bends, cut, steps, straight);
        check(verb(g1, "KartBattle.LastBurst") == verb(host, "KartBattle.LastBurst"), "...the same tree, bent the same, on every runtime");
        check(bends > 0 && cut < straight, "...and the block bends it: fewer of its steps end in the block than its tree set down straight");
    }

    // The newer arms, on open ground: the mine left alone, the shockwave, the
    // homing rocket, the two a kart carries, and oil.
    verb(host, "KartBattle.Abandon");
    run(2);
    check(verb(g1, "KartBattle.Recolour", "3") == "ok", "back in the lobby a look can change again");
    run(4);
    check(verb(host, "KartBattle.Configure", "range 60") == "ok" && start() == "ok", "a round on the range");
    run(2);
    check(verb(g1, "KartBattle.Recolour", "4") == "refused" && verb(g1, "KartBattle.Rename", "late") == "refused", "...and not in a round");
    {
        const Kart r0 = where(host, "host"), r1 = where(host, "g1"), r2 = where(host, "g2");
        std::printf("        (host %.1f,%.1f  g1 %.1f,%.1f  g2 %.1f,%.1f)\n", r0.x, r0.z, r1.x, r1.z, r2.x, r2.z);
        check(near(r0.z, -20, 0.01f) && near(r1.x, 12, 0.01f) && near(r1.z, 20, 0.01f) && near(r2.z, -24.0f, 0.01f), "the three where the range puts them");

        // A mine behind the host, two short of g2: not driven
        // on, it lies there twenty seconds and then bursts by itself.
        verb(host, "KartBattle.Give", "host mine");
        fire(host);
        run(940);
        const std::string lying = verb(host, "KartBattle.LastBurst");
        check(where(host, "g2").hp == 100, "a mine nobody drives on lies there...");
        run(70);
        const int mined = 100 - where(host, "g2").hp;
        std::printf("        (the mine, timed out, did %d to g2 two off: %s)\n", mined, verb(host, "KartBattle.LastBurst").c_str());
        // (How much of its fire reaches g2 is the tree's: the session's own, so a run's.)
        check(verb(host, "KartBattle.LastBurst") != lying && mined <= 30 && where(host, "host").hp == 100,
              "...twenty seconds, then bursts by itself: nothing is left lying on the map");

        // The shockwave: a ring out from the host. g2, behind it, is hurt
        // and thrown back; g1, far off, is not reached.
        verb(host, "KartBattle.Give", "host shockwave");
        const Kart w0 = where(host, "g2");
        fire(host);
        run(25);
        const Kart w1 = where(host, "g2");
        std::printf("        (g2 %d -> %d hp, z %.2f -> %.2f)\n", w0.hp, w1.hp, w0.z, w1.z);
        check(w0.hp - w1.hp == 15 && w1.z < w0.z - 1.0f, "a shockwave takes 15 off a kart its ring passes and throws it outward");
        check(where(host, "g1").hp == 100 && where(host, "host").hp == 100, "...not the kart that set it off, nor one beyond its reach");
        run(60);

        // The homing rocket: fired up the lane, it turns to g1, twelve to the side.
        verb(host, "KartBattle.Give", "host homing");
        fire(host);
        for (int i = 0; i < 120 && where(host, "g1").hp == 100; ++i) run(2);
        const int homed = 100 - where(host, "g1").hp;
        std::printf("        (the homing rocket did %d)\n", homed);
        check(homed >= 20 && homed <= 30, "a homing rocket turns to the kart ahead of it and bursts on it: up to 30");
        check(where(host, "host").weapon == "homing" && where(host, "host").ammo == 1, "...one left");

        // A smoke bomb: a cloud round g2, and a dagger. Driven up against the
        // host from behind, every fifth of a second it takes 8.
        verb(host, "KartBattle.Give", "g2 smoke");
        run(2);
        proposed(g2, ' ', true); run(1); proposed(g2, ' ', false); run(1);
        const Kart d0 = where(host, "g2"), v0 = where(host, "host");
        check(d0.dagger > 300 && d0.weapon == "-", "a smoke bomb gives its kart a dagger for eight seconds");
        proposed(g2, 'W', true);
        int touching = 0;
        for (int t = 0; t < 400 && touching < 60; ++t)
        {
            run(1);
            const Kart a = where(host, "g2"), h = where(host, "host");
            if (std::hypot(a.x - h.x, a.z - h.z) < 2.6f) ++touching;
        }
        proposed(g2, 'W', false);
        // ...and g2 stopped there, behind the host, for the slick to come.
        proposed(g2, 'S', true);
        for (int t = 0; t < 150 && where(host, "g2").speed > 0.05f; ++t) run(1);
        proposed(g2, 'S', false);
        const Kart v1 = where(host, "host");
        std::printf("        (the host, up against g2's dagger %d ticks: %d -> %d hp)\n", touching, v0.hp, v1.hp);
        check(touching >= 10 && v0.hp - v1.hp >= 8 && (v0.hp - v1.hp) % 8 == 0, "a kart up against it is cut, 8 a fifth of a second");
        run(500);
        check(where(host, "g2").dagger == 0 && where(g1, "g2").dagger == 0, "...and the dagger is gone after eight seconds, on every runtime");

        // Two are carried. A power-up goes to the spare when there is a
        // weapon in hand; the same weapon again is more of it.
        verb(host, "KartBattle.Give", "host rocket");
        verb(host, "KartBattle.Give", "host mine pickup");
        Kart c = where(host, "host");
        check(c.weapon == "rocket" && c.ammo == 2 && c.spare == "mine" && c.spare_ammo == 2, "a power-up with a weapon in hand is the spare");
        verb(host, "KartBattle.Give", "host rocket pickup");
        verb(host, "KartBattle.Give", "host mine pickup");
        c = where(host, "host");
        check(c.weapon == "rocket" && c.ammo == 4 && c.spare == "mine" && c.spare_ammo == 4, "the same weapon again is more of it: in hand, or the spare");
        verb(host, "KartBattle.Give", "host rocket pickup");
        verb(host, "KartBattle.Give", "host rocket pickup");
        verb(host, "KartBattle.Give", "host oil pickup");
        run(2);
        c = where(host, "host");
        check(c.ammo == 6 && c.spare == "oil" && c.spare_ammo == 1, "...three power-ups' worth at most; a different one takes the spare's place");
        check(where(g1, "host").spare == "oil" && where(g2, "host").ammo == 6, "...on every runtime");
        verb(host, "KartBattle.Give", "host boost");
        c = where(host, "host");
        check(c.weapon == "boost" && c.ammo == 1 && c.spare == "oil", "(a weapon given outright is in hand, the spare kept)");

        // Spent, the spare comes to hand: the boost carries the host up the
        // lane, and then Space is the oil -- a slick behind it.
        key(host, 'W', true);
        fire(host);
        c = where(host, "host");
        check(c.weapon == "oil" && c.ammo == 1 && c.spare == "-", "the weapon in hand spent, the spare comes to hand");
        run(60);
        const float slick = where(host, "host").z - 3.0f;
        fire(host);
        check(where(host, "host").weapon == "-", "...and is spent in its turn");
        run(100);
        key(host, 'W', false);
        // g2 up the lane after it, over the slick: a third of its pedal.
        proposed(g2, 'W', true);
        // Gains over ten ticks: the pedal's push a tick is under the speed's
        // tenth, which is all a Kart line says.
        float before = 0.0f, onIt = 0.0f;
        std::vector<float> speeds;
        std::vector<int> slips;
        int slipped = 0;
        for (int i = 0; i < 300; ++i)
        {
            run(1);
            const Kart k = where(host, "g2");
            speeds.push_back(k.speed); slips.push_back(k.slip > 0 ? 1 : 0);
            if (k.slip > 0) ++slipped;
            const size_t n = speeds.size();
            if (n > 10)
            {
                const float gain = speeds[n - 1] - speeds[n - 11];
                bool all = true, none = true;
                for (size_t j = n - 10; j < n; ++j) { all = all && slips[j]; none = none && !slips[j]; }
                if (all) onIt = std::max(onIt, gain);
                else if (none && !slipped && k.speed < 12.0f) before = std::max(before, gain);
            }
            if (k.z > slick + 6.0f) break;
        }
        proposed(g2, 'W', false);
        std::printf("        (a slick at z %.1f: g2 gained %.3f u/s in ten ticks before it, %.3f at most on it, %d ticks slipping)\n", slick, before, onIt, slipped);
        check(slipped > 10 && onIt < before * 0.6f, "a kart on a slick has oil on its tyres: its pedal does a third of what it did");
        check(where(host, "host").slip == 0, "...and not the kart that dropped it");
    }

    // -- 9. the same battle --------------------------------------------------
    std::cout << "\n-- 9. the same battle --\n";
    run(250 - status(host).tick % 250);
    const std::string hh = verb(host, "KartBattle.Hash"), h1s = verb(g1, "KartBattle.Hash"), h2s = verb(g2, "KartBattle.Hash");
    std::printf("        (host %s / g1 %s / g2 %s)\n", hh.c_str(), h1s.c_str(), h2s.c_str());
    check(hh == h1s && hh == h2s, "each guest at the host's tick is the host's battle, to the bit");
    const std::string sh = verb(host, "KartBattle.Snapshot");
    check(sh == verb(g1, "KartBattle.Snapshot") && sh == verb(g2, "KartBattle.Snapshot") && sh.compare(0, 2, "0 ") != 0,
          "...and checked itself where the host's trace said its hash");
    check(verb(g1, "KartBattle.Note").find("differs") == std::string::npos && verb(g2, "KartBattle.Note").find("differs") == std::string::npos,
          "...and never found itself out of step");

    // The streams wound up as a closing script winds up what it detached:
    // each told to stop, then joined.
    auto& detached = ETCS::DetachedRegistry::getInstance();
    for (auto& [id, script] : detached.list()) detached.interrupt(id);
    for (int i = 0; i < 2500 && !detached.all_finished(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(detached.all_finished(), "the four streams end when told to");
    detached.join_all();
    check(where(host, "g1").slot < 0 && where(host, "g2").slot < 0 && verb(host, "KartBattle.Here").empty(),
          "each seat's stream ending is its leave: both karts back in the pit, nobody here");
    for (ETCS::Entity* b : battle) verb(b, "KartBattle.Delete");
    ETCS::PendingUnloadRegistry::getInstance().join_all();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
