// KartVehiclesTesterLoader.cc
//
// A VEHICLE IS A SCRIPT, AND A RIDE IS A LINE. The arena runs one script a
// vehicle (KartProvider/scripts/kart_vehicle_*.etcs): its name, its numbers
// (KartBattle::Tune) and its look. A driver picks one in the lobby, by the
// menu or by Ride, and the pick is a `ride` line, so every runtime drives the
// same. What is held to account, on three runtimes of one session:
//
//   1. THE SCRIPTS. The arena has the kart, two bikes and two trucks, in
//      order. A name taken refuses its vehicle and the Tune and VehicleLook
//      after it; a number out of its range refuses the line.
//   2. THE RIDE. The menu's vehicle row steps through them, either way round;
//      Ride names one. Either is a line every runtime has; a name nobody made
//      is refused, and so is any ride outside the lobby. A vehicle rides on
//      its own tyres: the bike's body where the kart's is, the truck's higher.
//   3. THE PEDAL. The same driver, the same keys, from the same spawn: the
//      moto is the quickest off the line and to the highest top, then the
//      bike, the kart, the pickup and the truck, slowest of all.
//   3b. THE PEDAL AND THE WHEEL BY LEVEL. A touch stick's pedal (Pedal 8)
//      is W; half of it pushes about half as hard; all of it the other way
//      reverses. Its wheel (Wheel 8) is D; half of it turns half as far; -8
//      turns the other way as far.
//   3c. DRAWN BETWEEN TICKS. Frames at twice the tick rate: the rows move on
//      every other frame, the drawn kart (Drawn) on every one, behind the rows
//      by a tick at most; a guest draws it moving on every frame too.
//   3d. THE EYE AND A BLOCK. A turned block behind the kart, taller than the
//      eye rides: the eye comes in in front of it.
//   3e. A GUEST'S TICKS IN BURSTS. The host's ticks reach a guest unevenly,
//      up to five at once with gaps between: the guest still draws the kart
//      moving on every frame, at an even pace.
//   4. THE TYRES. Off the pedal at 12 u/s, a wheel held half a second: the
//      kart and both trucks grip (nothing across is left) and both bikes
//      slide, leaning into the turn and the other way for the other turn; the
//      kart and the trucks never lean. A bike's two tyres take its slide's
//      heat, the hidden pair none. At its top on a held wheel the kart slides
//      and both trucks grip.
//   4b. THE LEAN. A bike leans in as its wheel is held, all the way only
//      after a fifth of a second or so, and when the wheel is let go it
//      settles upright, a slide or not.
//   4c. THE OIL. A slick oils whichever tyres touch it -- its own kart's too,
//      once that kart has left it: backing on, the rear tyres, then all four.
//   5. THE WEIGHT. A bike and a truck head on, off the pedal: the truck,
//      about nine bikes' weight, carries on, and the bike goes back.
//   6. THE SAME BATTLE. With guests on a moto and a truck driving and
//      turning, and the weight's crash, each guest at the host's tick has its
//      hash.
//   7. A LATE ARENA. A guest that joins before its arena is made (a slow
//      page) holds the host's lines until it is: then it has the others on
//      their moto and truck, and the host's hash.
//   7b. PUT RIGHT. A guest knocked out of step (its arena made again under
//      the round, every kart back at a spawn) differs at the host's next
//      hash, replays its trace, and is the host's again, and says so.
//
//   ./Run_KartVehiclesTesterLoader
//
// No display, no GPU, no network.

#include "../ETCS.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <cstdio>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

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

// KartBattle.Kart: "<slot> <x> <z> <hx> <hz> <speed> <hp> <weapon> <ammo> <y> <air|ground> <wheel> <slide>
// <spare> <spare ammo> <oil ticks> <dagger ticks> <vehicle> <lean> <oiled, a digit a tyre>".
struct Kart { int slot = -1; float x = 0, z = 0, speed = 0, y = 0, slide = 0, lean = 0, wheel = 0; int oil = 0; std::string vehicle, oiled; };
static Kart where(ETCS::Entity* b, const std::string& who)
{
    Kart k;
    const std::string row = verb(b, "KartBattle.Kart", who);
    if (row == "-") return k;
    std::istringstream in(row);
    std::string w;
    float f;
    int n;
    in >> k.slot >> k.x >> k.z >> f >> f >> k.speed >> n >> w >> n >> k.y >> w >> k.wheel >> k.slide >> w >> n >> k.oil >> n >> k.vehicle >> k.lean >> k.oiled;
    return k;
}
// KartBattle.Tyres: per tyre (fr fl rr rl) "<heat> <rope> <shown> <darkest>".
static void heats(ETCS::Entity* b, const std::string& who, float h[4])
{
    std::istringstream in(verb(b, "KartBattle.Tyres", who));
    float f;
    int n;
    for (int t = 0; t < 4; ++t) in >> h[t] >> f >> n >> f;
}
// KartBattle.Status: "<tick> <lines> <mode> <me> <slot> <phase> ...".
struct Status { uint64_t tick = 0, lines = 0; std::string mode, me; int slot = -1; std::string phase; };
static Status status(ETCS::Entity* b)
{
    Status s;
    std::istringstream in(verb(b, "KartBattle.Status"));
    in >> s.tick >> s.lines >> s.mode >> s.me >> s.slot >> s.phase;
    return s;
}

int main(int, char**)
{
    WIRE_CONTEXT();
    std::cout << "=== A vehicle is a script, and a ride is a line ===\n";

    ETCS::Entity* battle[3] = {};
    for (auto& b : battle)
    {
        ETCS::Entity* world = ETCS::spawn_entity("RenderProvider", "Scene3D", env, loader);
        b = ETCS::spawn_entity("KartProvider", "KartBattle", env, loader);
        if (!world || !b) { std::printf("FAILED (could not spawn)\n"); return 1; }
        verb(world, "Scene3D.Create", "1, 1, 1");
        verb(world, "Scene3D.SetVisible", "0");
        verb(b, "KartBattle.Create");
        verb(b, "KartBattle.BindWorld", std::to_string(world->getRID()));
        ETCS::ExecutionContext arena(&root, &ctx);
        arena.names["anchor"] = ETCS::NameBinding{ world->getRID(), "RenderProvider", "Scene3D" };
        arena.names["battle"] = ETCS::NameBinding{ b->getRID(), "KartProvider", "KartBattle" };
        std::istringstream in("run KartProvider/scripts/kart_arena.etcs anchor=anchor battle=battle\n");
        if (!ETCS::run_script(in, std::string(ETCS_ACE_ROOT) + "/modules/kart_world.etcs", arena)) { std::printf("FAILED (the arena script)\n"); return 1; }
    }
    ETCS::Entity* host = battle[0];
    ETCS::Entity* g1 = battle[1];
    ETCS::Entity* g2 = battle[2];

    // -- 1. the scripts --------------------------------------------------------
    std::cout << "\n-- 1. the scripts --\n";
    check(verb(host, "KartBattle.Vehicles") == "kart\nmoto\nbike\npickup\ntruck\n", "the arena has the kart, two bikes and two trucks, in order");
    // The same on every runtime, as a script would be.
    bool taken = true, ranged = true, took = true;
    for (ETCS::Entity* b : battle)
    {
        taken = taken && verb(b, "KartBattle.AddVehicle", "moto") == "refused" && verb(b, "KartBattle.AddVehicle", "kart") == "refused"
                      && verb(b, "KartBattle.Tune", "top 99") == "refused" && verb(b, "KartBattle.VehicleLook", "0") == "refused";
        took = took && verb(b, "KartBattle.AddVehicle", "test") == "ok";
        for (const char* bad : { "tyres 3 0 1", "slide 1 0.1", "slide 0.5 1.5", "top -1", "top 0", "grip 20", "warp 9", "top 3x" })
            ranged = ranged && verb(b, "KartBattle.Tune", bad) == "refused";
        took = took && verb(b, "KartBattle.Tune", "top 30") == "ok" && verb(b, "KartBattle.Tune", "slide 0.5 1") == "ok"
                    && verb(b, "KartBattle.Tune", "tyres 2 0 0.75") == "ok";
    }
    check(taken, "a name taken (another's, or the kart's) is refused, and the Tune and VehicleLook after it");
    check(ranged, "a number out of its range, or one too few, or a word nobody tunes, is refused");
    check(took && verb(g2, "KartBattle.Vehicles") == "kart\nmoto\nbike\npickup\ntruck\ntest\n", "one more, tuned, after them");

    // -- the session, on a strip of its own: the host in a corner, facing the
    // far one down the arena's diagonal; the guests in the two others, off it.
    for (ETCS::Entity* b : battle)
    {
        verb(b, "KartBattle.AddMap", "strip");
        verb(b, "KartBattle.AddSpawn", "-85, -85");
        // Behind the host's spawn, turned, taller than the eye rides: for 3d.
        verb(b, "KartBattle.AddBlock", "-90.5, -90.5, 3, 3, 6, 45");
        verb(b, "KartBattle.AddSpawn", "80, -80");
        verb(b, "KartBattle.AddSpawn", "-80, 80");
    }
    verb(host, "KartBattle.Host", "host 0 t1");
    verb(g1, "KartBattle.Join", "g1 host");
    verb(g2, "KartBattle.Join", "g2 host");
    {
        ETCS::ExecutionContext wire(&root, &ctx);
        auto bind = [&](const char* n, ETCS::Entity* e) { wire.names[n] = ETCS::NameBinding{ e->getRID(), "KartProvider", "KartBattle" }; };
        bind("host", host); bind("g1", g1); bind("g2", g2);
        std::istringstream lines(
            "detach NetworkProvider/scripts/session_follow.etcs src=host dst=g1\n"
            "detach NetworkProvider/scripts/session_emit.etcs src=g1 dst=host\n"
            "detach NetworkProvider/scripts/session_follow.etcs src=host dst=g2\n"
            "detach NetworkProvider/scripts/session_emit.etcs src=g2 dst=host\n");
        check(ETCS::run_script(lines, std::string(ETCS_ACE_ROOT) + "/modules/kart_wiring.etcs", wire), "the streams are wired with the page's scripts");
    }
    auto settle = [&]() {
        const uint64_t t = status(host).tick;
        auto behind = [&](ETCS::Entity* g) { const Status s = status(g); return s.mode == "guest" && s.tick != t; };
        for (int i = 0; i < 400 && (behind(g1) || behind(g2)); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
        const uint64_t before = status(host).lines;
        verb(b, "KartBattle.Key", std::to_string(k) + " " + (down ? "1" : "0"));
        if (b != host) for (int i = 0; i < 400 && status(host).lines == before; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    auto rides = [&](ETCS::Entity* on, const char* who, const char* what) {
        for (int i = 0; i < 200 && where(on, who).vehicle != what; ++i) run(2);
        return where(on, who).vehicle == what;
    };
    for (int i = 0; i < 300 && (where(host, "g1").slot < 0 || where(host, "g2").slot < 0); ++i) run(2);
    check(verb(host, "KartBattle.Configure", "strip 300") == "ok", "three seated, on the strip");

    // -- 2. the ride -----------------------------------------------------------
    std::cout << "\n-- 2. the ride --\n";
    check(where(host, "g1").vehicle == "kart" && where(g2, "host").vehicle == "kart", "every driver begins in the kart");
    // A guest's menu: vehicle, colour, name, settings -- the first row is its vehicle.
    key(g2, 262, true); key(g2, 262, false);
    check(rides(host, "g2", "moto") && rides(g1, "g2", "moto"), "g2's menu, right: the moto, on every runtime");
    key(g1, 263, true); key(g1, 263, false);
    check(rides(host, "g1", "test") && rides(g2, "g1", "test"), "g1's, left: round past the kart to the last");
    key(g1, 263, true); key(g1, 263, false);
    check(rides(host, "g1", "truck") && rides(g2, "g1", "truck"), "...and left again: the truck");
    check(verb(host, "KartBattle.Ride", "hovercraft") == "refused", "a vehicle nobody made is refused");
    check(verb(host, "KartBattle.Ride", "moto") == "ok" && rides(g1, "host", "moto"), "Ride names one: the host's moto, everywhere");
    {
        const float moto = where(host, "g2").y;   // parked on the strip, as every runtime has it
        verb(host, "KartBattle.Ride", "bike");
        rides(g2, "host", "bike");
        const float bike = where(g2, "host").y;
        verb(host, "KartBattle.Ride", "truck");
        rides(g2, "host", "truck");
        const float truck = where(g2, "host").y;
        std::printf("        (ride heights: the moto %.3f, the bike %.3f, the truck %.3f)\n", moto, bike, truck);
        check(std::fabs(moto - 0.25f) < 1e-3f && std::fabs(bike - 0.25f) < 1e-3f && std::fabs(truck - 0.34f) < 1e-3f,
              "a vehicle rides on its tyres: the moto's and the bike's body at 0.25, the truck's at 0.34");
    }

    // -- 3. the pedal ------------------------------------------------------------
    std::cout << "\n-- 3. the pedal --\n";
    // The host from the strip's first spawn, down the diagonal of an empty arena.
    struct Pace { float at2 = 0, at8 = 0; };
    auto pace = [&](const char* v) {
        Pace p;
        verb(host, "KartBattle.Ride", v);
        rides(host, "host", v);
        start();
        run(2);
        key(host, 'W', true);
        run(100); p.at2 = where(host, "host").speed;
        run(300); p.at8 = where(host, "host").speed;
        key(host, 'W', false);
        verb(host, "KartBattle.Abandon");
        run(2);
        std::printf("        (%s: %.1f u/s at 2 s, %.1f at 8 s)\n", v, p.at2, p.at8);
        return p;
    };
    const Pace kart = pace("kart"), moto = pace("moto"), bike = pace("bike"), pickup = pace("pickup"), truck = pace("truck");
    check(moto.at2 > bike.at2 && bike.at2 > kart.at2 && kart.at2 > pickup.at2 && pickup.at2 > truck.at2,
          "off the line: the moto, the bike, the kart, the pickup, the truck");
    check(std::fabs(kart.at8 - 22.0f) < 0.2f && std::fabs(moto.at8 - 34.0f) < 0.2f && std::fabs(bike.at8 - 26.0f) < 0.2f
              && std::fabs(pickup.at8 - 17.0f) < 0.2f,
          "each at its own top by 8 s: the kart 22, the moto 34, the bike 26, the pickup 17");
    check(truck.at8 > 16.0f && truck.at8 < 17.0f, "...and the truck, on the softest push, not yet at its 17");
    check(start() == "ok" && (run(2), verb(host, "KartBattle.Ride", "kart")) == "refused",
          "no ride outside the lobby");
    verb(host, "KartBattle.Abandon");
    run(2);

    // -- 3b. the pedal and the wheel by level -------------------------------------
    std::cout << "\n-- 3b. the pedal and the wheel by level --\n";
    // A touch pad's pedal: Pedal <level>, -8..8, a `drive` line like a key's.
    // The kart from the strip's spawn a second on the whole pedal, then on half.
    auto second = [&](const char* how) {
        start();
        run(2);
        if (std::string(how) == "W") key(host, 'W', true); else verb(host, "KartBattle.Pedal", how);
        run(50);
        const float v = where(host, "host").speed;
        if (std::string(how) == "W") key(host, 'W', false); else verb(host, "KartBattle.Pedal", "0");
        verb(host, "KartBattle.Abandon");
        run(2);
        return v;
    };
    verb(host, "KartBattle.Ride", "kart");
    rides(host, "host", "kart");
    const float whole = second("W"), eight = second("8"), half = second("4"), back = second("-8");
    std::printf("        (after a second: W %.2f u/s, Pedal 8 %.2f, Pedal 4 %.2f, Pedal -8 %.2f)\n", whole, eight, half, back);
    check(std::fabs(eight - whole) < 0.01f, "the whole pedal is a key's");
    check(half > 0.3f * whole && half < 0.6f * whole, "half of it pushes about half as hard (less, low down: the pedal's curve climbs with the speed)");
    check(back > 4.0f, "...and all of it down the other way, from a stand, reverses (the speed is its size)");
    // The wheel the same way: Wheel <level>, a touch stick's. Rolling at 6
    // u/s, held a fifth of a second: D, Wheel 8, Wheel 4, Wheel -8.
    auto turned = [&](const char* how) {
        start();
        run(2);
        key(host, 'W', true);
        for (int i = 0; i < 300 && where(host, "host").speed < 6; ++i) run(1);
        key(host, 'W', false);
        if (std::string(how) == "D") key(host, 'D', true); else verb(host, "KartBattle.Wheel", how);
        run(10);
        const float w = where(host, "host").wheel;
        if (std::string(how) == "D") key(host, 'D', false); else verb(host, "KartBattle.Wheel", "0");
        verb(host, "KartBattle.Abandon");
        run(2);
        return w;
    };
    const float wd = turned("D"), w8 = turned("8"), w4 = turned("4"), wl = turned("-8");
    std::printf("        (the wheel after a fifth of a second: D %.3f, Wheel 8 %.3f, Wheel 4 %.3f, Wheel -8 %.3f rad)\n", wd, w8, w4, wl);
    check(wd > 0.0f && std::fabs(w8 - wd) < 1e-4f, "the whole wheel is a key's");
    check(std::fabs(w4 - wd * 0.5f) < 0.02f, "half of it turns half as far");
    check(std::fabs(wl + wd) < 1e-4f, "...and the other way, the other way as far");

    // -- 3c. drawn between ticks -------------------------------------------------
    std::cout << "\n-- 3c. drawn between ticks --\n";
    // The host's kart at speed, then frames of 10 ms -- two to a tick: the
    // rows move only on the frames a tick falls in, the drawn kart on every
    // frame, about half a tick's way each, never ahead of the rows. On a
    // guest, whose ticks come two at a time, the same kart is drawn moving on
    // every frame too.
    {
        auto xz = [](const std::string& t, float& x, float& z) {
            std::istringstream in(t); float y; return static_cast<bool>(in >> x >> y >> z); };
        start();
        run(2);
        key(host, 'W', true);
        run(100);
        int rowStill = 0, drawnStill = 0, drawnMoved = 0, ahead = 0, guestStill = 0, guestMoved = 0, guestNone = 0;
        float rowMost = 0.0f, drawnMost = 0.0f, lagMost = 0.0f;
        Kart was = where(host, "host");
        float dx0 = 0.0f, dz0 = 0.0f, gx0 = 0.0f, gz0 = 0.0f;
        bool first = true;
        for (int i = 0; i < 40; ++i)
        {
            verb(host, "KartBattle.Frame", "10");
            settle();
            verb(g1, "KartBattle.Frame", "10");
            const Kart now = where(host, "host");
            float dx, dz, gx, gz;
            const bool drawn = xz(verb(host, "KartBattle.Drawn", "host"), dx, dz);
            const bool gdrawn = xz(verb(g1, "KartBattle.Drawn", "host"), gx, gz);
            if (!gdrawn) ++guestNone;
            if (!first && drawn)
            {
                const float row = std::hypot(now.x - was.x, now.z - was.z), step = std::hypot(dx - dx0, dz - dz0);
                if (row < 1e-4f) ++rowStill;
                rowMost = std::max(rowMost, row);
                if (step < 1e-4f) ++drawnStill; else ++drawnMoved;
                drawnMost = std::max(drawnMost, step);
                // Behind the rows, along the way the kart goes, by a tick at most.
                const float vx = now.x - was.x, vz = now.z - was.z, lag = std::hypot(now.x - dx, now.z - dz);
                if ((now.x - dx) * vx + (now.z - dz) * vz < -1e-3f) ++ahead;
                lagMost = std::max(lagMost, lag);
                if (gdrawn) { if (std::hypot(gx - gx0, gz - gz0) < 1e-4f) ++guestStill; else ++guestMoved; }
            }
            was = now; first = !drawn;
            if (drawn) { dx0 = dx; dz0 = dz; }
            if (gdrawn) { gx0 = gx; gz0 = gz; }
        }
        key(host, 'W', false);
        verb(host, "KartBattle.Abandon");
        run(2);
        std::printf("        (40 frames of 10 ms: the rows still on %d, a tick's step %.3f at most; drawn still on %d, moved on %d, a frame's "
                    "step %.3f at most, %.3f behind the rows at most, ahead %d; on g1 drawn still on %d, moved on %d, not drawn on %d)\n",
                    rowStill, rowMost, drawnStill, drawnMoved, drawnMost, lagMost, ahead, guestStill, guestMoved, guestNone);
        check(rowStill >= 15 && drawnStill == 0 && drawnMoved >= 35, "the rows move on every other frame, the drawn kart on every frame");
        check(drawnMost < 0.75f * rowMost && lagMost <= rowMost * 1.05f + 1e-3f && ahead == 0,
              "...about half a tick's way a frame, behind the rows and never by more than a tick");
        check(guestNone == 0 && guestStill <= 4 && guestMoved >= 30, "a guest, its ticks two at a time, draws it moving on (almost) every frame too");
    }

    // -- 3d. the eye and a block ---------------------------------------------------
    std::cout << "\n-- 3d. the eye and a block --\n";
    // The host at its spawn, a turned block behind it on its boom, taller
    // than the eye: the eye comes in to this side of the block.
    {
        verb(host, "KartBattle.Ride", "kart");
        rides(host, "host", "kart");
        start();
        run(2);
        for (int i = 0; i < 20; ++i) verb(host, "KartBattle.Frame", "100");
        std::istringstream in(verb(host, "KartBattle.Eye"));
        float ex, ey, ez;
        in >> ex >> ey >> ez;
        const Kart k = where(host, "host");
        const float boom = std::hypot(ex - k.x, ez - k.z);
        // The block, turned 45: |u| and |v| in its frame, against its half 1.5.
        const float dx = ex + 90.5f, dz = ez + 90.5f, c = 0.70710678f;
        const float u = std::fabs(dx * c - dz * c), v = std::fabs(dx * c + dz * c);
        std::printf("        (the kart at %.2f %.2f, its eye at %.2f %.2f %.2f: %.2f behind it, %.2f %.2f off the block's middle in its frame)\n",
                    k.x, k.z, ex, ey, ez, boom, u, v);
        check(boom < 7.0f && (u > 1.5f || v > 1.5f), "a block behind the kart, taller than the eye: the eye comes in in front of it (short of its 7.5), not inside it");
        verb(host, "KartBattle.Abandon");
        run(2);
    }

    // -- 3e. a guest's ticks in bursts -------------------------------------------
    std::cout << "\n-- 3e. a guest's ticks in bursts --\n";
    // Frames of 10 ms on g1; the host's clock moved only now and then, by the
    // frames since (1 to 10 of them), so its ticks reach g1 in uneven bursts,
    // up to five at once. After a second to settle, g1 draws the host's kart
    // moving every frame, and no frame's step more than half again the usual
    // one. (Aimed at kBatch behind the newest tick, as it was, the drawn
    // tick caught up with the newest on these and stood still.)
    {
        start();
        run(2);
        key(host, 'W', true);
        run(100);
        const int gaps[] = { 1, 10, 2, 10, 1, 9, 3, 10 };
        int gap = 0, since = 0, still = 0;
        float gx0 = 0.0f, gz0 = 0.0f;
        std::vector<float> steps;
        for (int i = 0; i < 260; ++i)
        {
            if (++since == gaps[gap])
            {
                verb(host, "KartBattle.Frame", std::to_string(since * 10));
                settle();
                since = 0; gap = (gap + 1) % 8;
            }
            verb(g1, "KartBattle.Frame", "10");
            std::istringstream in(verb(g1, "KartBattle.Drawn", "host"));
            float gx, gy, gz;
            if (!(in >> gx >> gy >> gz)) { ++still; continue; }
            if (i >= 100)
            {
                const float step = std::hypot(gx - gx0, gz - gz0);
                if (step < 1e-4f) ++still; else steps.push_back(step);
            }
            gx0 = gx; gz0 = gz;
        }
        key(host, 'W', false);
        verb(host, "KartBattle.Abandon");
        run(2);
        std::vector<float> sorted = steps;
        std::sort(sorted.begin(), sorted.end());
        const float usual = sorted.empty() ? 0.0f : sorted[sorted.size() / 2], most = sorted.empty() ? 0.0f : sorted.back();
        std::printf("        (160 frames of 10 ms on g1: still on %d, moved on %zu; a frame's step usually %.3f, at most %.3f)\n",
                    still, steps.size(), usual, most);
        check(still == 0 && steps.size() == 160 && most <= usual * 1.5f, "a guest whose ticks come in bursts draws the kart moving every frame, at an even pace");
    }

    // -- 4. the tyres ------------------------------------------------------------
    std::cout << "\n-- 4. the tyres --\n";
    // Up to `speed`, off the pedal, a wheel held half a second one way and
    // then the other: the most it slides and its lean at the end of each.
    struct Turn { float slide = 0, right = 0, left = 0; float heat[4] = {}; };
    auto turn = [&](const char* v, float speed, int hold) {
        Turn t;
        verb(host, "KartBattle.Ride", v);
        rides(host, "host", v);
        start();
        run(2);
        key(host, 'W', true);
        for (int i = 0; i < 600 && where(host, "host").speed < speed; ++i) run(1);
        if (hold == 0) key(host, 'W', false);
        key(host, 'D', true);
        for (int i = 0; i < 25; ++i) { run(1); t.slide = std::max(t.slide, std::fabs(where(host, "host").slide)); }
        t.right = where(host, "host").lean;
        heats(host, "host", t.heat);
        key(host, 'D', false);
        key(host, 'A', true);
        for (int i = 0; i < 25; ++i) { run(1); t.slide = std::max(t.slide, std::fabs(where(host, "host").slide)); }
        t.left = where(host, "host").lean;
        key(host, 'A', false);
        key(host, 'W', false);
        verb(host, "KartBattle.Abandon");
        run(2);
        std::printf("        (%s at %.0f: slides %.2f u/s at most, leans %.2f then %.2f; heat %.2f %.2f %.2f %.2f)\n",
                    v, speed, t.slide, t.right, t.left, t.heat[0], t.heat[1], t.heat[2], t.heat[3]);
        return t;
    };
    const Turn k12 = turn("kart", 12, 0), m12 = turn("moto", 12, 0), b12 = turn("bike", 12, 0),
               p12 = turn("pickup", 12, 0), t12 = turn("truck", 12, 0);
    // Gripping, the tyres pull out all of the way across: nothing slides.
    check(k12.slide == 0.0f && p12.slide == 0.0f && t12.slide == 0.0f && m12.slide > 0.0f && b12.slide > 0.0f,
          "at 12 u/s on a held wheel the kart and both trucks grip; both bikes slide");
    check(m12.right > 0.3f && m12.left < -0.3f && b12.right > 0.3f && b12.left < -0.3f,
          "...leaning into the turn, and the other way for the other");
    check(k12.right == 0.0f && k12.left == 0.0f && p12.right == 0.0f && t12.right == 0.0f, "the kart and the trucks never lean");
    check(m12.heat[1] == 0.0f && m12.heat[3] == 0.0f && m12.heat[0] > 0.0f && m12.heat[2] > 0.0f
              && b12.heat[1] == 0.0f && b12.heat[3] == 0.0f && b12.heat[0] + b12.heat[2] > 0.0f,
          "a bike's two tyres take its slide's heat; the hidden pair none");
    const Turn ktop = turn("kart", 21.9f, 1), ptop = turn("pickup", 16.9f, 1), ttop = turn("truck", 16.9f, 1);
    check(ktop.slide > 0.0f && ptop.slide == 0.0f && ttop.slide == 0.0f, "at its top on a held wheel the kart slides and both trucks grip");

    // -- 4b. the lean ------------------------------------------------------------
    std::cout << "\n-- 4b. the lean --\n";
    // Up to 18 u/s, off the pedal; the wheel held a second and a fifth, then
    // let go for as long: the lean every 3 ticks.
    struct Lean { float at[20] = {}, free[20] = {}; };
    auto lean = [&](const char* v) {
        Lean l;
        verb(host, "KartBattle.Ride", v);
        rides(host, "host", v);
        start();
        run(2);
        key(host, 'W', true);
        for (int i = 0; i < 600 && where(host, "host").speed < 18; ++i) run(1);
        key(host, 'W', false);
        key(host, 'D', true);
        for (int i = 0; i < 60; ++i) { run(1); if (i % 3 == 2) l.at[i / 3] = where(host, "host").lean; }
        key(host, 'D', false);
        for (int i = 0; i < 60; ++i) { run(1); if (i % 3 == 2) l.free[i / 3] = where(host, "host").lean; }
        verb(host, "KartBattle.Abandon");
        run(2);
        std::printf("        (%s held:", v);
        for (float f : l.at) std::printf(" %.2f", f);
        std::printf("\n         %s let go:", v);
        for (float f : l.free) std::printf(" %.2f", f);
        std::printf(")\n");
        return l;
    };
    const Lean ml = lean("moto"), bl = lean("bike");
    auto builds = [](const Lean& l) { const float most = l.at[19]; return most > 0.5f && l.at[0] < 0.25f * most && l.at[1] < 0.75f * most && l.at[4] > 0.95f * most; };
    auto settles = [](const Lean& l) {
        bool down = l.free[0] > 0.5f * l.at[19];
        for (int i = 1; i < 5; ++i) down = down && l.free[i] < l.free[i - 1];
        return down && std::fabs(l.free[11]) < 0.05f;
    };
    check(builds(ml) && builds(bl), "a held wheel leans a bike in gradually: under a quarter of the way at 0.06 s, all of it by 0.3 s");
    check(settles(ml) && settles(bl), "let go, it settles back upright, steadily, within 0.7 s -- though it is still sliding");

    // -- 4c. the oil ---------------------------------------------------------------
    std::cout << "\n-- 4c. the oil --\n";
    // The kart drops a slick standing (it is on it: its rear tyres), drives
    // off it, stops, and backs over it again.
    {
        verb(host, "KartBattle.Ride", "kart");
        rides(host, "host", "kart");
        start();
        run(2);
        key(host, 'W', true); run(40); key(host, 'W', false);
        key(host, 'S', true);
        for (int i = 0; i < 200 && where(host, "host").speed > 0.05f; ++i) run(1);
        key(host, 'S', false);
        run(5);
        verb(host, "KartBattle.Give", "host oil");
        key(host, ' ', true); run(1); key(host, ' ', false);
        run(10);
        const Kart dropped = where(host, "host");
        key(host, 'W', true); run(60); key(host, 'W', false);
        key(host, 'S', true);
        for (int i = 0; i < 200 && where(host, "host").speed > 0.05f; ++i) run(1);
        const Kart off = where(host, "host");
        bool rear_only = false, all = false;
        for (int i = 0; i < 400 && !all; ++i)
        {
            run(1);
            const Kart k = where(host, "host");
            rear_only = rear_only || k.oiled == "0011";
            all = all || k.oiled == "1111";
        }
        key(host, 'S', false);
        std::printf("        (just dropped: oil %d, tyres %s; driven off: %s; backed over it: rear alone %s, all %s)\n",
                    dropped.oil, dropped.oiled.c_str(), off.oiled.c_str(), rear_only ? "yes" : "no", all ? "yes" : "no");
        check(dropped.oil == 0 && dropped.oiled == "0000", "a kart standing on the slick it just dropped has no oil from it");
        check(off.oiled == "0000" && rear_only && all, "backing over it after it left it, its own oil: on the rear tyres first, then all four");
        verb(host, "KartBattle.Abandon");
        run(2);
    }

    // -- 5. the weight ------------------------------------------------------------
    std::cout << "\n-- 5. the weight --\n";
    // g1 on the truck and g2 on the bike, spawned in opposite corners facing
    // the middle: head on. Off the pedal a little before they meet.
    verb(g2, "KartBattle.Ride", "bike");
    rides(host, "g2", "bike");
    verb(host, "KartBattle.Ride", "kart");
    rides(host, "host", "kart");
    start();
    run(2);
    // The truck's way: from its corner, whichever the seats gave it, to the middle.
    const Kart ts = where(host, "g1");
    const float tl = std::hypot(ts.x, ts.z), wx = -ts.x / tl, wz = -ts.z / tl;
    key(g1, 'W', true); key(g2, 'W', true);
    auto apart = [&]() { const Kart a = where(host, "g1"), b = where(host, "g2"); return std::hypot(a.x - b.x, a.z - b.z); };
    for (int i = 0; i < 1000 && apart() > 6.0f; ++i) run(1);
    key(g1, 'W', false); key(g2, 'W', false);
    const Kart t0 = where(host, "g1"), b0 = where(host, "g2");
    float closest = apart();
    for (int i = 0; i < 75; ++i) { run(1); closest = std::min(closest, apart()); }
    const Kart t1 = where(host, "g1"), b1 = where(host, "g2");
    const float truckOn = (t1.x - t0.x) * wx + (t1.z - t0.z) * wz;
    const float bikeOn = (b1.x - b0.x) * wx + (b1.z - b0.z) * wz;
    std::printf("        (met at %.2f apart; over 1.5 s the truck went %.2f on, the bike %.2f on, the truck's way)\n", closest, truckOn, bikeOn);
    check(closest < 2.6f && truckOn > 0.0f && bikeOn > 0.0f,
          "they meet, and both go the truck's way after: it carries on and the bike goes back");
    verb(host, "KartBattle.Abandon");
    run(2);

    // -- 6. the same battle --------------------------------------------------------
    std::cout << "\n-- 6. the same battle --\n";
    verb(g2, "KartBattle.Ride", "moto");
    rides(host, "g2", "moto");
    start();
    run(2);
    key(g1, 'W', true); key(g2, 'W', true); key(host, 'W', true);
    run(120);
    key(g2, 'D', true); key(g1, 'A', true);
    run(60);
    const Kart m = where(host, "g2");
    key(g2, 'D', false); key(g1, 'A', false);
    run(40);
    key(g1, 'W', false); key(g2, 'W', false); key(host, 'W', false);
    run(10);
    std::printf("        (g2's moto leaned %.2f; hashes %s | %s | %s)\n", m.lean, verb(host, "KartBattle.Hash").c_str(),
                verb(g1, "KartBattle.Hash").c_str(), verb(g2, "KartBattle.Hash").c_str());
    check(m.vehicle == "moto" && m.lean > 0.3f && where(host, "g1").vehicle == "truck", "a guest on the moto leans in its turn; a guest on the truck drives");
    const std::string h = verb(host, "KartBattle.Hash");
    check(h == verb(g1, "KartBattle.Hash") && h == verb(g2, "KartBattle.Hash"), "each guest at the host's tick has its hash");

    // -- 7. a late arena -------------------------------------------------------------
    std::cout << "\n-- 7. a late arena --\n";
    // g3 joins and follows before its arena script has run, as a slow page
    // does; its lines wait for Ready, and then it is the same battle.
    ETCS::Entity* g3 = nullptr;
    {
        ETCS::Entity* world = ETCS::spawn_entity("RenderProvider", "Scene3D", env, loader);
        g3 = ETCS::spawn_entity("KartProvider", "KartBattle", env, loader);
        verb(world, "Scene3D.Create", "1, 1, 1");
        verb(world, "Scene3D.SetVisible", "0");
        verb(g3, "KartBattle.Create");
        verb(g3, "KartBattle.BindWorld", std::to_string(world->getRID()));
        verb(g3, "KartBattle.Join", "g3 host");
        ETCS::ExecutionContext wire(&root, &ctx);
        wire.names["host"] = ETCS::NameBinding{ host->getRID(), "KartProvider", "KartBattle" };
        wire.names["g3"] = ETCS::NameBinding{ g3->getRID(), "KartProvider", "KartBattle" };
        std::istringstream lines("detach NetworkProvider/scripts/session_follow.etcs src=host dst=g3\n"
                                 "detach NetworkProvider/scripts/session_emit.etcs src=g3 dst=host\n");
        ETCS::run_script(lines, std::string(ETCS_ACE_ROOT) + "/modules/kart_wiring.etcs", wire);
        key(host, 'W', true);
        run(40);
        const Status early = status(g3);
        // ...and only now its arena, maps and vehicles -- with what the others
        // were given here (the test vehicle, the tunes, the strip) before its
        // Ready, the arena script's last line, as a page's scripts would.
        ETCS::ExecutionContext arena(&root, &ctx);
        arena.names["anchor"] = ETCS::NameBinding{ world->getRID(), "RenderProvider", "Scene3D" };
        arena.names["battle"] = ETCS::NameBinding{ g3->getRID(), "KartProvider", "KartBattle" };
        const std::string path = std::string(ETCS_ACE_ROOT) + "/modules/KartProvider/scripts/kart_arena.etcs";
        std::ifstream file(path);
        std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        const size_t ready = text.rfind("battle.Ready()");
        if (ready != std::string::npos) text.erase(ready, std::string("battle.Ready()").size());
        std::istringstream in(text);
        bool made = ready != std::string::npos && ETCS::run_script(in, path, arena);
        made = made && verb(g3, "KartBattle.AddVehicle", "test") == "ok" && verb(g3, "KartBattle.Tune", "top 30") == "ok"
                    && verb(g3, "KartBattle.Tune", "slide 0.5 1") == "ok" && verb(g3, "KartBattle.Tune", "tyres 2 0 0.75") == "ok";
        verb(g3, "KartBattle.AddMap", "strip");
        verb(g3, "KartBattle.AddSpawn", "-85, -85");
        verb(g3, "KartBattle.AddBlock", "-90.5, -90.5, 3, 3, 6, 45");
        verb(g3, "KartBattle.AddSpawn", "80, -80");
        verb(g3, "KartBattle.AddSpawn", "-80, 80");
        verb(g3, "KartBattle.Ready");
        // To the host's next hash and past it, g3 caught up.
        const uint64_t past = (status(host).tick / 250 + 1) * 250 + 5;
        run(static_cast<uint32_t>(past - status(host).tick));
        key(host, 'W', false);
        run(2);
        for (int i = 0; i < 400 && status(g3).tick != status(host).tick; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const std::string note = verb(g3, "KartBattle.Note");
        std::printf("        (before its arena: g3 at tick %llu with %llu lines; after: tick %llu, g1 on %s, g2 on %s; note \"%s\")\n",
                    static_cast<unsigned long long>(early.tick), static_cast<unsigned long long>(early.lines),
                    static_cast<unsigned long long>(status(g3).tick), where(g3, "g1").vehicle.c_str(), where(g3, "g2").vehicle.c_str(), note.c_str());
        check(made && early.tick == 0, "a guest whose arena is not made yet applies none of the host's lines");
        check(where(g3, "g1").vehicle == "truck" && where(g3, "g2").vehicle == "moto" && verb(g3, "KartBattle.Hash") == verb(host, "KartBattle.Hash"),
              "...and once it is, it has the others on their moto and truck, and the host's hash");
    }

    // -- 7b. put right ---------------------------------------------------------------
    std::cout << "\n-- 7b. put right --\n";
    // Ready again on g1 under the round: the first map put back, every kart
    // at a spawn -- what a slow page's arena did to it before the lines waited.
    {
        key(host, 'W', true);
        run(20);
        verb(g1, "KartBattle.Ready");
        const Kart moved = where(g1, "host"), real = where(host, "host");
        const bool knocked = std::hypot(moved.x - real.x, moved.z - real.z) > 1.0f;
        const uint64_t past = (status(host).tick / 250 + 1) * 250 + 5;
        const auto t0 = std::chrono::steady_clock::now();
        run(static_cast<uint32_t>(past - status(host).tick));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        key(host, 'W', false);
        run(2);
        const Kart after = where(g1, "host"), truth = where(host, "host");
        const std::string note = verb(g1, "KartBattle.Note");
        std::printf("        (knocked %.1f off; at tick %llu of %llu lines, %.0f ms to the hash and past it; then %.3f off; note \"%s\")\n",
                    std::hypot(moved.x - real.x, moved.z - real.z), static_cast<unsigned long long>(status(host).tick),
                    static_cast<unsigned long long>(status(host).lines), ms, std::hypot(after.x - truth.x, after.z - truth.z), note.c_str());
        check(knocked && verb(g1, "KartBattle.Hash") == verb(host, "KartBattle.Hash") && std::hypot(after.x - truth.x, after.z - truth.z) < 1e-3f
              && note.find("put right") != std::string::npos,
              "a guest knocked out of step replays its trace at the host's next hash, is the host's again, and says so");
    }

    // Wound up as a closing script winds up what it detached.
    auto& detached = ETCS::DetachedRegistry::getInstance();
    for (auto& [id, script] : detached.list()) detached.interrupt(id);
    for (int i = 0; i < 2500 && !detached.all_finished(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(detached.all_finished(), "the streams end when told to");
    detached.join_all();
    for (ETCS::Entity* b : battle) verb(b, "KartBattle.Delete");
    if (g3) verb(g3, "KartBattle.Delete");
    ETCS::PendingUnloadRegistry::getInstance().join_all();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
