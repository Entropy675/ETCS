// KartTesterLoader.cc
//
// ONE BATTLE, THREE RUNTIMES' WORTH OF IT, IN ONE PROCESS. Three worlds built
// by the same arena script (KartProvider/scripts/kart_arena.etcs), three
// battles on them: the host's judges, two guests follow its record. What a
// link carries between runtimes is carried here by local Ledgers and the same
// stream scripts the page runs: each guest's proposals ledger is authored as
// that guest (what a link's name does), the host judges both, and both guests
// follow the host's record. What is held to account:
//
//   1. THE LOBBY. Three drivers, three karts, each at the spawn furthest from
//      the others, facing the middle, whole and unarmed -- and parked: W moves
//      nobody, and a guest sets nothing.
//   2. A ROUND. Started, its settings are locked. The pedal takes the host
//      toward the middle and the bowl's bumper stops it; a moving kart turns
//      with its wheels, one standing still does not, its front wheels seen
//      turned and the rear square. The host abandons it: back to the lobby.
//   3. THE SETTINGS. In the lobby the host's `config` brings out another map
//      at once and respawns everyone on it.
//   4. A ROUND ABANDONED WITH A SCORE, ONE COMPLETED. An elimination in a
//      round the host abandons is discarded with it. Then: the sniper dead on
//      takes a whole kart, back three seconds later. A rocket bursts on a kart for up to 40; a fuse hurts
//      all but its own kart; the minigun fires while held, a tap between two
//      ticks still fires, a boost outruns a kart's top; a power-up hands over
//      a weapon. The clock runs out into the results, then the lobby, which
//      keeps the round's scores; the next round starts them afresh.
//   5. THE GROUND. On the skatepark a kart climbs a ramp at the ramp's
//      height, flies off its lip and lands beyond it; another driven at a lip
//      from the drop side is stopped by it.
//   6. A MAP FROM A SEED. The same layout on every runtime; a new seed, a new
//      layout, still the same everywhere; every kart on the ground at a spawn.
//   7. THE SAME BATTLE. Each guest at the host's tick has the host's hash --
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

// KartBattle.Kart: "<slot> <x> <z> <hx> <hz> <speed> <hp> <weapon> <ammo> <y> <air|ground>".
struct Kart { int slot = -1; float x = 0, z = 0, hx = 0, hz = 0, speed = 0; int hp = -1; std::string weapon; int ammo = 0; float y = 0; std::string air; };
static Kart where(ETCS::Entity* b, const std::string& who)
{
    Kart k;
    const std::string row = verb(b, "KartBattle.Kart", who);
    if (row == "-") return k;   // not driving (a failed >> would read a 0)
    std::istringstream in(row);
    in >> k.slot >> k.x >> k.z >> k.hx >> k.hz >> k.speed >> k.hp >> k.weapon >> k.ammo >> k.y >> k.air;
    return k;
}
// KartBattle.Status: "<tick> <seq> <mode> <me> <slot> <phase> <secs left> <map> <round secs> <layout>".
struct Status { uint64_t tick = 0, seq = 0; std::string mode, me; int slot = -1; std::string phase; int left = 0; std::string map; int secs = 0; uint64_t layout = 0; };
static Status status(ETCS::Entity* b)
{
    Status s;
    std::istringstream in(verb(b, "KartBattle.Status"));
    in >> s.tick >> s.seq >> s.mode >> s.me >> s.slot >> s.phase >> s.left >> s.map >> s.secs >> s.layout;
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
    check(verb(host, "KartBattle.Maps") == "bowl\npillars\ncrossroads\nhills\nskatepark\nrandom\n",
          "the arena has its six maps, in order");

    // The record and a proposals ledger per guest, wired as the page wires
    // them -- with the page's own stream scripts.
    ETCS::Entity* rec = ETCS::spawn_entity("NetworkProvider", "Ledger", env, loader);
    ETCS::Entity* p1  = ETCS::spawn_entity("NetworkProvider", "Ledger", env, loader);
    ETCS::Entity* p2  = ETCS::spawn_entity("NetworkProvider", "Ledger", env, loader);
    verb(rec, "Ledger.Author", "host");
    verb(p1, "Ledger.Author", "g1");
    verb(p2, "Ledger.Author", "g2");
    verb(host, "KartBattle.Host", std::to_string(rec->getRID()) + " " + std::to_string(p1->getRID()) + " 0 host 0 t1");
    verb(g1, "KartBattle.Join", "0 g1 host");
    {
        ETCS::ExecutionContext wire(&root, &ctx);
        auto bind = [&](const char* n, ETCS::Entity* e, const char* mod, const char* tag)
        { wire.names[n] = ETCS::NameBinding{ e->getRID(), mod, tag }; };
        bind("rec", rec, "NetworkProvider", "Ledger"); bind("p1", p1, "NetworkProvider", "Ledger"); bind("p2", p2, "NetworkProvider", "Ledger");
        bind("host", host, "KartProvider", "KartBattle"); bind("g1", g1, "KartProvider", "KartBattle"); bind("g2", g2, "KartProvider", "KartBattle");
        std::istringstream lines(
            "detach KartProvider/scripts/kart_judge.etcs src=p1 dst=host\n"
            "detach KartProvider/scripts/kart_judge.etcs src=p2 dst=host\n"
            "detach KartProvider/scripts/kart_follow.etcs src=rec dst=g1\n"
            "detach KartProvider/scripts/kart_emit.etcs src=g1 dst=p1\n");
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
    auto key = [&](ETCS::Entity* b, int k, bool down) {
        verb(b, "KartBattle.Key", std::to_string(k) + " " + (down ? "1" : "0"));
    };
    // A guest's key reaches the host by the proposals stream: wait for it to
    // be judged (the host's record seq moves) before the clock runs on.
    auto proposed = [&](ETCS::Entity* b, int k, bool down) {
        const uint64_t before = status(host).seq;
        key(b, k, down);
        for (int i = 0; i < 400 && status(host).seq == before; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    auto fire = [&](ETCS::Entity* b) { key(b, ' ', true); run(1); key(b, ' ', false); run(1); };

    // -- 1. the lobby ---------------------------------------------------------
    std::cout << "\n-- 1. the lobby --\n";
    // g1 joins over its link before g2 does, so who sits where is the record's
    // order: the host first, then g1 furthest from it, then g2 furthest from both.
    for (int i = 0; i < 200 && where(host, "g1").slot < 0; ++i) run(2);
    verb(g2, "KartBattle.Join", "0 g2 host");
    {
        ETCS::ExecutionContext wire(&root, &ctx);
        wire.names["g2"] = ETCS::NameBinding{ g2->getRID(), "KartProvider", "KartBattle" };
        wire.names["p2"] = ETCS::NameBinding{ p2->getRID(), "NetworkProvider", "Ledger" };
        wire.names["rec"] = ETCS::NameBinding{ rec->getRID(), "NetworkProvider", "Ledger" };
        // After the Join, as kart_join.etcs has it: a Join starts the battle
        // afresh, so a record followed before it would be lines lost.
        std::istringstream lines("detach KartProvider/scripts/kart_follow.etcs src=rec dst=g2\n"
                                 "detach KartProvider/scripts/kart_emit.etcs src=g2 dst=p2\n");
        ETCS::run_script(lines, std::string(ETCS_ACE_ROOT) + "/modules/kart_wiring.etcs", wire);
    }
    for (int i = 0; i < 200 && where(host, "g2").slot < 0; ++i) run(2);
    const Kart h0 = where(host, "host"), a0 = where(host, "g1"), b0 = where(host, "g2");
    std::printf("        (host %.1f,%.1f  g1 %.1f,%.1f  g2 %.1f,%.1f)\n", h0.x, h0.z, a0.x, a0.z, b0.x, b0.z);
    check(h0.slot == 0 && a0.slot == 1 && b0.slot == 2, "three drivers, each in a kart of their own");
    check(near(h0.x, 0, 0.01f) && near(h0.z, -24, 0.01f) && near(h0.hz, 1, 1e-3f) && h0.speed == 0.0f && near(h0.y, 0.25f, 1e-3f),
          "the host at the bowl's first spawn, on the ground, facing the middle, still");
    check(near(a0.z, 24, 0.01f) && near(a0.hz, -1, 1e-3f), "g1 at the spawn furthest from the host, facing it across the middle");
    check(near(b0.x, 24, 0.01f) && near(b0.z, 0, 0.01f) && near(b0.hx, -1, 1e-3f), "g2 at the spawn furthest from both");
    check(h0.hp == 100 && a0.weapon == "-" && status(g2).phase == "lobby", "whole and unarmed, in the lobby");
    key(host, 'W', true);
    run(50);
    key(host, 'W', false);
    check(near(where(host, "host").z, -24, 1e-3f), "in the lobby the karts are parked: W moves nobody");
    check(verb(g1, "KartBattle.Configure", "pillars 60") == "refused" && verb(g1, "KartBattle.StartRound") == "refused",
          "a guest sets nothing: the map, the length and the round are the host's");

    // -- 2. a round, abandoned ------------------------------------------------
    std::cout << "\n-- 2. a round, abandoned --\n";
    check(verb(host, "KartBattle.StartRound") == "ok", "the host starts a round on the bowl");
    run(2);
    check(status(g1).phase == "playing" && status(g2).phase == "playing", "...on every runtime");
    check(verb(host, "KartBattle.Configure", "crossroads 60") == "refused" && status(host).map == "bowl",
          "its settings are locked: no config while it runs");
    key(host, 'W', true);
    run(60);
    const Kart h1 = where(host, "host");
    check(h1.z - h0.z > 5.0f && h1.speed > 9.0f && near(h1.x, 0, 0.05f), "W: toward the middle, quickly, in a straight line");
    run(240);
    key(host, 'W', false);
    const Kart h2 = where(host, "host");
    std::printf("        (stopped at z %.2f)\n", h2.z);
    check(h2.z > -11.3f && h2.z < -10.3f, "the bumper in its way stops it: solid meets solid");
    proposed(g1, 'W', true);
    run(40);
    proposed(g1, 'A', true);
    run(25);
    const Kart a1 = where(host, "g1");
    proposed(g1, 'A', false);
    proposed(g1, 'W', false);
    check(a1.hx < -0.3f && a1.speed > 5.0f, "A while moving: the wheels turn left, and the kart with them");
    proposed(g2, 'A', true);
    run(50);
    const Kart b1 = where(host, "g2");
    proposed(g2, 'A', false);
    check(near(b1.hx, -1, 1e-3f) && b1.speed == 0.0f, "A standing still: the wheels turn, the kart does not");
    {
        // The wheels seen: g2's four, found as the drawn things at its kart's
        // corners, each axle (its cylinder's axis) across the kart -- the
        // front two turned left by the lock, the rear two square.
        Causal_* wc = static_cast<Causal_*>(worlds[0]->getInterfacePointer(ETCS::Buffer("Causal")));
        std::vector<Causal_*> by;
        wc->Near(Fixed::From(b1.x), Fixed::From(0.25), Fixed::From(b1.z), Fixed::From(1.4), by);
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
            const float along = (r.x.ToFloat() - b1.x) * b1.hx + (r.z.ToFloat() - b1.z) * b1.hz;
            if (along > 0) { front += a; ++nf; } else { rear += a; ++nr; }
        }
        std::printf("        (axles off square: front %.3f rad, rear %.3f rad, %d+%d wheels)\n", nf ? front / nf : -1.f, nr ? rear / nr : -1.f, nf, nr);
        check(nf == 2 && nr == 2 && std::fabs(front / nf - 0.45f) < 0.02f && rear / nr < 0.01f,
              "...and the front wheels are seen turned to the lock, the rear ones square");
    }
    verb(host, "KartBattle.Abandon");
    run(2);
    check(status(g1).phase == "lobby" && near(where(host, "host").z, -24, 0.01f),
          "the host abandons the round: back in the lobby on every runtime, everyone respawned, parked");

    // -- 3. the settings -----------------------------------------------------
    std::cout << "\n-- 3. the settings --\n";
    check(verb(host, "KartBattle.Configure", "crossroads 30") == "ok", "in the lobby the host sets crossroads, thirty seconds");
    run(2);
    const Status s3 = status(g2);
    const Kart h3 = where(host, "host"), a3 = where(host, "g1"), b3 = where(host, "g2");
    std::printf("        (host %.1f,%.1f  g1 %.1f,%.1f  g2 %.1f,%.1f)\n", h3.x, h3.z, a3.x, a3.z, b3.x, b3.z);
    check(s3.map == "crossroads" && s3.secs == 30 && s3.phase == "lobby", "...and the map is out at once, on every runtime");
    check(near(h3.z, -27, 0.01f) && near(a3.z, 27, 0.01f) && near(b3.x, -27, 0.01f) && near(a3.hz, -1, 1e-3f),
          "...with everyone respawned on it: the host and g1 face to face down a road");
    {
        // The bowl's bumper the host stopped at is gone from where it stood.
        Causal_* wc = static_cast<Causal_*>(worlds[1]->getInterfacePointer(ETCS::Buffer("Causal")));
        std::vector<Causal_*> by;
        wc->Near(Fixed::Zero(), Fixed::From(0.4), Fixed::From(-9.0), Fixed::From(0.5), by);
        bool bump = false;
        // A bumper by its size: a reach of |(2, 0.4, 0.6)|.
        for (Causal_* c : by)
            if (c->Solid().shape == CausalSolid::Box && std::fabs(c->Order4().radius.ToFloat() - 2.126f) < 0.01f) bump = true;
        check(!bump, "...and the last map's blocks are out of the way (here on g1's world)");
    }

    // -- 4. a round, abandoned with a score, then one completed ---------------
    std::cout << "\n-- 4. a round, abandoned with a score, then one completed --\n";
    verb(host, "KartBattle.StartRound");
    run(2);
    verb(host, "KartBattle.Give", "host sniper");
    fire(host);
    const int counted = score(g1, "host").elims;
    verb(host, "KartBattle.Abandon");
    run(2);
    std::printf("        (an elimination counted: %d; after the abandon: %d)\n", counted, score(g2, "host").elims);
    check(counted == 1 && status(g2).phase == "lobby" && score(g2, "host").elims == 0 && score(g2, "g1").deaths == 0,
          "an abandoned round's elimination is discarded with it, on every runtime");
    check(verb(host, "KartBattle.StartRound") == "ok", "the host starts the round again");
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
    check(a5.hp == 100 && near(a5.z, 27, 0.01f) && score(host, "g1").state == "alive",
          "three seconds later g1 is back, whole, at the spawn furthest from everyone");

    verb(host, "KartBattle.Give", "host rocket");
    fire(host);
    check(where(host, "host").weapon == "rocket" && where(host, "host").ammo == 1, "a rocket fired, one left");
    for (int i = 0; i < 60 && where(host, "g1").hp == 100; ++i) run(2);
    const int rocket = 100 - where(host, "g1").hp;
    std::printf("        (the rocket did %d)\n", rocket);
    check(rocket >= 37 && rocket <= 40, "it bursts on the kart it meets: up to 40 at the burst's centre");

    verb(host, "KartBattle.Give", "host fuse");
    fire(host);
    run(130);
    check(where(host, "host").hp == 100, "a fuse burns out on the kart that lit it, and does it no harm");

    proposed(g2, 'W', true);
    std::string got = "-";
    for (int i = 0; i < 80 && got == "-"; ++i) { run(2); got = where(host, "g2").weapon; }
    proposed(g2, 'W', false);
    std::printf("        (g2 drove over a box and got a %s)\n", got.c_str());
    check(got != "-" && where(g2, "g2").weapon == got, "a power-up hands over a weapon off the record's dice, the same everywhere");

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
    run(60);
    const float boosted = where(host, "host").speed;
    key(host, 'W', false);
    std::printf("        (boosted to %.1f)\n", boosted);
    check(boosted > 17.0f, "a boost takes a kart past its own top speed");

    run(static_cast<uint32_t>(status(host).left) * 50 + 2);
    const Status s6 = status(g1);
    check(s6.phase == "results" && score(g1, "host").elims == 1, "the clock runs out into the results, the scores standing");
    run(302);
    check(status(g2).phase == "lobby" && score(g2, "host").elims == 1,
          "...then the lobby, which keeps the round's scores (an abandoned one's it did not)");
    verb(host, "KartBattle.StartRound");
    run(2);
    const Score e7 = score(g1, "host"), d7 = score(g1, "g1");
    check(e7.elims == 0 && d7.deaths == 0 && status(g1).phase == "playing", "the next round starts the scores afresh");
    verb(host, "KartBattle.Abandon");
    run(2);

    // -- 5. the ground ---------------------------------------------------------
    std::cout << "\n-- 5. the ground --\n";
    check(verb(host, "KartBattle.Configure", "skatepark 60") == "ok" && verb(host, "KartBattle.StartRound") == "ok",
          "a round on the skatepark: two jumps down the middle");
    run(2);
    const Kart h5 = where(host, "host");
    check(near(h5.z, -27, 0.01f) && near(h5.hz, 1, 1e-3f), "the host at the foot of the near jump, facing it");
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
            if (k.air == "ground" && k.z > -16.0f && k.z < -12.0f && onRampErr < 0.0f)
                onRampErr = std::fabs(k.y - (0.25f + 1.5f * (k.z + 17.0f) / 6.0f));
            if (k.air == "air" && !flew) { flew = true; liftZ = k.z; }
            if (flew) topY = std::max(topY, k.y);
            if (flew && k.air == "ground") { landed = true; landZ = k.z; }
        }
        key(host, 'W', false);
        key(host, 'S', true);
        run(80);
        key(host, 'S', false);
        std::printf("        (on the ramp %.3f off its height; off the lip at z %.2f, up to y %.2f, down at z %.2f)\n",
                    onRampErr, liftZ, topY, landZ);
        check(onRampErr >= 0.0f && onRampErr < 0.03f, "up the ramp, the kart rides at the ramp's height");
        check(flew && liftZ > -11.6f && liftZ < -10.4f && topY > 1.8f, "...leaves the ground at the lip and climbs higher still");
        check(landed && landZ > -9.0f, "...and lands beyond it");
    }
    {
        // g1 down the middle the other way, at the far jump's lip from below.
        proposed(g1, 'W', true);
        run(200);
        const Kart k = where(host, "g1");
        proposed(g1, 'W', false);
        std::printf("        (g1 stopped at z %.2f, y %.2f)\n", k.z, k.y);
        check(k.z > 17.4f && k.z < 18.6f && near(k.y, 0.25f, 0.01f) && k.speed < 0.5f,
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
          "...the same layout on every runtime, its seed in the record");
    check(verb(host, "KartBattle.Reroll") == "ok", "a new seed");
    run(2);
    const std::string l2 = verb(host, "KartBattle.Layout");
    std::printf("        (%s)\n", l2.c_str());
    check(l2 != l1 && l2 == verb(g1, "KartBattle.Layout") && l2 == verb(g2, "KartBattle.Layout"),
          "...a new layout, still the same everywhere");
    {
        std::istringstream in(l2);
        std::string name; uint64_t seed; int blocks, ramps, hills, spawns, pickups;
        in >> name >> seed >> blocks >> ramps >> hills >> spawns >> pickups;
        bool grounded = true;
        for (const char* who : { "host", "g1", "g2" })
        {
            const Kart k = where(host, who);
            grounded = grounded && k.air == "ground" && near(k.y, 0.25f, 0.01f);
        }
        check(spawns == 8 && pickups >= 1 && blocks + ramps + hills >= 3 && grounded,
              "...with things on it, eight spawns, and every kart on level ground at one");
    }
    check(verb(host, "KartBattle.StartRound") == "ok", "a round on it");
    proposed(g1, 'W', true);
    proposed(g2, 'D', true);
    proposed(g2, 'W', true);
    key(host, 'W', true);
    run(300);
    key(host, 'W', false);
    proposed(g1, 'W', false);
    proposed(g2, 'W', false);
    proposed(g2, 'D', false);

    // -- 7. the same battle --------------------------------------------------
    std::cout << "\n-- 7. the same battle --\n";
    run(250 - status(host).tick % 250);
    const std::string hh = verb(host, "KartBattle.Hash"), h1s = verb(g1, "KartBattle.Hash"), h2s = verb(g2, "KartBattle.Hash");
    std::printf("        (host %s / g1 %s / g2 %s)\n", hh.c_str(), h1s.c_str(), h2s.c_str());
    check(hh == h1s && hh == h2s, "each guest at the host's tick is the host's battle, to the bit");
    const std::string sh = verb(host, "KartBattle.Snapshot");
    check(sh == verb(g1, "KartBattle.Snapshot") && sh == verb(g2, "KartBattle.Snapshot") && sh.compare(0, 2, "0 ") != 0,
          "...and advertises the same snapshot");

    // The streams wound up as a closing script winds up what it detached:
    // each told to stop, then joined.
    auto& detached = ETCS::DetachedRegistry::getInstance();
    for (auto& [id, script] : detached.list()) detached.interrupt(id);
    for (int i = 0; i < 2500 && !detached.all_finished(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(detached.all_finished(), "the six streams end when told to");
    detached.join_all();
    for (ETCS::Entity* b : battle) verb(b, "KartBattle.Delete");
    ETCS::PendingUnloadRegistry::getInstance().join_all();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
