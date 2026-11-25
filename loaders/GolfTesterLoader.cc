// GolfTesterLoader.cc
//
// ONE MATCH, TWO RUNTIMES' WORTH OF IT, IN ONE PROCESS. Two worlds built by
// the same script (GolfProvider/scripts/golf_world.etcs), two matches on
// them: the host's is the authority, the guest's mirrors its trace. What a
// link carries between runtimes -- the guest's seat into the host's match and
// the host's trace out of it -- is carried here by the page's own stream
// scripts (NetworkProvider/scripts/session_*.etcs) between the two matches
// directly, with no surface between. What is held to account:
//
//   0. THE COURSES. classic, boosters, terraces and a random one, each
//      hand-made hole sound (GolfCourse::Faults): a cup cut into a level
//      green, a tee, pads, springs and spots each on a surface.
//   1. ALONE. Practising, a ball on the first tee at rest; a shot along a
//      pull moves it that way, and in the lobby nothing is counted.
//   2. THE HAND. A press on your ball pulls a shot back; letting go of the
//      RIGHT button while the left holds it puts it down -- the left's
//      release then shoots nothing; pressed and let go again, it shoots.
//   3. TWO PLAYERS. Hosted, the guest's seat comes on its stream: two balls
//      side by side on the tee, and each match has the other here. A guest
//      sets nothing. The host starts a round; a guest's shot is a stroke on
//      its card, taken by the host.
//   4. HOLED. A ball put just past the lip and nudged back drops in: fit
//      takes it into the cup, the hole is over for it in one more stroke,
//      and its ball leaves the course a moment later.
//   5. THE HOLE'S TIME. A ball still out when the clock runs down is scored
//      the most (the cap and one); then the next hole, everyone on its tee.
//   6. THE BOOSTERS. A pad pushes a ball rolling over it the way it points,
//      far past what its shot gave it; a spring throws one up; a ball shot
//      short of the gap is lost and goes back where it was struck, +1.
//   7. POWERS. A bumper put down is a slab on the hole; a black hole pulls a
//      ball toward it; a wind blows every ball its way; a teleport moves only
//      its user's ball, and only so far; a tornado's lightning strikes, and a
//      ball near a strike is thrown; a power is used once.
//   8. THE ROUND. Every hole played out, the results, then the lobby, the
//      round's totals kept on the board; a course from a seed is the same on
//      both runtimes.
//   9. THE SAME MATCH. The guest at the host's tick has the host's hash --
//      the balls' rows, the cards, the dice -- and checked itself at every
//      hash the host's trace said. Its seat's stream ending is its `leave`:
//      the host takes its ball off, and nobody is here any more.
//
//   ./Run_GolfTesterLoader
//
// No display, no GPU, no network.

#include "../ETCS.h"

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
// GolfMatch.Ball: "<slot> <x> <y> <z> <speed> <strokes> <total> <state> <power> <resting|moving> <spare>".
struct Ball { int slot = -1; float x = 0, y = 0, z = 0, speed = 0; int strokes = 0, total = 0; std::string state, power, rest, spare; };
static Ball ball(ETCS::Entity* g, const std::string& who)
{
    Ball b;
    const std::string row = verb(g, "GolfMatch.Ball", who);
    if (row == "-") return b;
    std::istringstream in(row);
    in >> b.slot >> b.x >> b.y >> b.z >> b.speed >> b.strokes >> b.total >> b.state >> b.power >> b.rest >> b.spare;
    return b;
}
// GolfMatch.Status: "<tick> <lines> <mode> <me> <slot> <phase> <secs left> <course> <hole> <holes> <hole secs> <layout>".
struct Status { uint64_t tick = 0, lines = 0; std::string mode, me; int slot = -1; std::string phase; int left = 0; std::string course; int hole = 0, holes = 0, secs = 0; uint64_t layout = 0; };
static Status status(ETCS::Entity* g)
{
    Status s;
    std::istringstream in(verb(g, "GolfMatch.Status"));
    in >> s.tick >> s.lines >> s.mode >> s.me >> s.slot >> s.phase >> s.left >> s.course >> s.hole >> s.holes >> s.secs >> s.layout;
    return s;
}
static int slabs(ETCS::Entity* g)
{
    std::istringstream in(verb(g, "GolfMatch.Layout"));
    std::string course; uint64_t layout = 0; int hole = 0, n = 0;
    in >> course >> layout >> hole >> n;
    return n;
}

int main(int, char**)
{
    WIRE_CONTEXT();
    std::cout << "=== One match, two runtimes' worth ===\n";

    ETCS::Entity* game[2] = {};
    for (int i = 0; i < 2; ++i)
    {
        ETCS::Entity* world = ETCS::spawn_entity("RenderProvider", "Scene3D", env, loader);
        ETCS::Entity* eye   = ETCS::spawn_entity("RenderProvider", "Camera3D", env, loader);
        game[i] = ETCS::spawn_entity("GolfProvider", "GolfMatch", env, loader);
        if (!world || !eye || !game[i]) { std::printf("FAILED (could not spawn)\n"); return 1; }
        verb(world, "Scene3D.Create", "1, 1, 1");
        verb(world, "Scene3D.SetVisible", "0");
        verb(eye, "Camera3D.Create", "1024, 768");
        verb(eye, "Camera3D.SetLens", "55, 0.05, 300");
        verb(eye, "Camera3D.SetScene", std::to_string(world->getRID()));
        verb(game[i], "GolfMatch.Create");
        ETCS::ExecutionContext made(&root, &ctx);
        made.names["anchor"] = ETCS::NameBinding{ world->getRID(), "RenderProvider", "Scene3D" };
        made.names["eye"]    = ETCS::NameBinding{ eye->getRID(), "RenderProvider", "Camera3D" };
        // The window's layout, as the boot makes it: the eye its root box.
        ETCS::Entity* hud = ETCS::spawn_entity("LayoutProvider", "Layout", env, loader);
        if (!hud) { std::printf("FAILED (could not spawn a layout)\n"); return 1; }
        verb(hud, "Layout.Create", "1024, 768");
        verb(hud, "Layout.AddBox", std::to_string(eye->getRID()) + ", 0, 1, 0.0, 1, 0.0");
        made.names["hud"]    = ETCS::NameBinding{ hud->getRID(), "LayoutProvider", "Layout" };
        made.names["game"]   = ETCS::NameBinding{ game[i]->getRID(), "GolfProvider", "GolfMatch" };
        std::istringstream in("run GolfProvider/scripts/golf_world.etcs anchor=anchor game=game\n"
                              "run GolfProvider/scripts/golf_play.etcs game=game world=anchor eye=eye layout=hud\n");
        if (!ETCS::run_script(in, std::string(ETCS_ACE_ROOT) + "/modules/golf_made.etcs", made)) { std::printf("FAILED (the course scripts)\n"); return 1; }
    }
    ETCS::Entity* host = game[0];
    ETCS::Entity* g1 = game[1];

    // -- 0. the courses --------------------------------------------------------
    std::cout << "\n-- 0. the courses --\n";
    check(verb(host, "GolfMatch.Maps") == "classic\nboosters\nterraces\nridge\nchannels\nrandom\n", "six courses, in order: classic, boosters, terraces, ridge, channels, random");
    {
        const std::string faults = verb(host, "GolfMatch.Faults");
        if (faults != "none") std::printf("        (%s)\n", faults.c_str());
        check(faults == "none", "...each hand-made hole sound: its cup cut into a level green, everything on it standing on something, and ground from its tee to its cup (or a pad or a spring to throw a ball)");
    }

    // -- 1. alone --------------------------------------------------------------
    std::cout << "\n-- 1. alone --\n";
    verb(host, "GolfMatch.Practice", "solo");
    verb(host, "GolfMatch.Run", "50");
    Ball b = ball(host, "solo");
    std::printf("        (on the tee: %.3f %.3f %.3f, %s)\n", b.x, b.y, b.z, b.rest.c_str());
    check(b.slot == 0 && std::fabs(b.y - 0.15f) < 0.01f && b.rest == "resting", "practising: a ball on the first tee, on the green, at rest");
    verb(host, "GolfMatch.Shoot", "0, 0, 1.5");
    verb(host, "GolfMatch.Run", "300");
    b = ball(host, "solo");
    std::printf("        (after a shot up the green: z %.2f, %s, strokes %d)\n", b.z, b.rest.c_str(), b.strokes);
    check(b.z > 4.0f && std::fabs(b.x + 1.4f) < 0.2f && b.strokes == 0, "a shot along +z rolls it up the green; in the lobby, uncounted");

    // -- 2. the hand -----------------------------------------------------------
    std::cout << "\n-- 2. the hand --\n";
    for (int i = 0; i < 3; ++i) verb(host, "GolfMatch.Frame", "16");   // the eye onto the ball
    const float z0 = ball(host, "solo").z;
    // In the lobby the menu is up, over the middle of the picture: a press
    // there is the menu's, and pulls nothing. M folds it away.
    verb(host, "GolfMatch.Press", "512, 384");
    verb(host, "GolfMatch.Drag", "512, 470");
    const std::string menus = verb(host, "GolfMatch.Pull");
    verb(host, "GolfMatch.Release", "512, 470");
    check(menus.find("pulling") == std::string::npos, "with the lobby's menu up, a press on it is the menu's: no ball is pulled through it");
    verb(host, "GolfMatch.Key", "77, 1");
    verb(host, "GolfMatch.Frame", "16");
    verb(host, "GolfMatch.Press", "512, 384");
    verb(host, "GolfMatch.Drag", "512, 470");
    const std::string pulling = verb(host, "GolfMatch.Pull");
    verb(host, "GolfMatch.RightPress", "512, 470");
    verb(host, "GolfMatch.RightRelease", "512, 470");
    const std::string down = verb(host, "GolfMatch.Pull");
    verb(host, "GolfMatch.Release", "512, 470");
    verb(host, "GolfMatch.Run", "100");
    std::printf("        (pulling: %s; after the right let go: %s)\n", pulling.c_str(), down.c_str());
    check(pulling.find("pulling") != std::string::npos, "a press on your ball and a drag pull a shot back");
    {
        // "x y z length mode": from a camera looking down 55 degrees the pull
        // lies along the ground the ball is on, but for its own rise off it
        // (35 degrees, as dragged) let through damped: for a pull this long, a little.
        float px = 0, py = 0, pz = 0, pl = 0;
        std::istringstream in(pulling);
        in >> px >> py >> pz >> pl;
        check(pl > 0.2f && py > 0.02f * pl && py < 0.15f * pl && pz > 0.9f * pl, "...along the green, its own rise let through damped: a little loft, no more");
    }
    check(down.find("down") != std::string::npos && std::fabs(ball(host, "solo").z - z0) < 0.01f,
          "letting go of the right button puts it down: the left's release shoots nothing");
    verb(host, "GolfMatch.Frame", "16");
    verb(host, "GolfMatch.Press", "512, 384");
    verb(host, "GolfMatch.Drag", "512, 470");
    verb(host, "GolfMatch.Release", "512, 470");
    verb(host, "GolfMatch.Run", "100");
    check(ball(host, "solo").z > z0 + 0.5f, "...pressed and let go again, it shoots: away from the camera, up the green");
    {
        // Loft is the camera brought down while the pull is held: the right
        // button with the left, the pointer up the picture fifty pixels
        // (0.3 of a radian).
        verb(host, "GolfMatch.Run", "250");
        verb(host, "GolfMatch.Frame", "16");
        verb(host, "GolfMatch.Press", "512, 384");
        verb(host, "GolfMatch.Drag", "512, 470");
        verb(host, "GolfMatch.RightPress", "512, 470");
        verb(host, "GolfMatch.RightDrag", "512, 420");
        float px = 0, py = 0, pz = 0, pl = 0;
        const std::string lofted = verb(host, "GolfMatch.Pull");
        std::istringstream in(lofted);
        in >> px >> py >> pz >> pl;
        std::printf("        (lofted: %s)\n", lofted.c_str());
        check(pl > 0.1f && py > 0.25f * pl && py < 0.35f * pl, "the camera lowered while a pull is held tips the shot up off the ground by as much");
        verb(host, "GolfMatch.RightDrag", "512, 520");
        std::istringstream again(verb(host, "GolfMatch.Pull"));
        again >> px >> py >> pz >> pl;
        check(py >= 0.0f && py < 0.15f * pl, "...and raised past where it was, back to the drag's own rise and no further");
        verb(host, "GolfMatch.RightRelease", "512, 520");
        verb(host, "GolfMatch.Release", "512, 520");
    }

    check(verb(host, "GolfMatch.Settings") == "look 1.000 zoom 1.000" && verb(host, "GolfMatch.Setting", "zoom 2") == "ok"
          && verb(host, "GolfMatch.Setting", "spin 2") == "refused" && verb(host, "GolfMatch.Settings") == "look 1.000 zoom 2.000",
          "a runtime's settings: the camera's look and zoom at their pace as made, and set");
    verb(host, "GolfMatch.Setting", "zoom 1");

    // -- 3. two players --------------------------------------------------------
    std::cout << "\n-- 3. two players --\n";
    verb(host, "GolfMatch.Host", "host 0 t1");
    verb(g1, "GolfMatch.Join", "g1 host");
    {
        ETCS::ExecutionContext wire(&root, &ctx);
        auto bind = [&](const char* n, ETCS::Entity* e) { wire.names[n] = ETCS::NameBinding{ e->getRID(), "GolfProvider", "GolfMatch" }; };
        bind("host", host); bind("g1", g1);
        std::istringstream lines("detach NetworkProvider/scripts/session_follow.etcs src=host dst=g1\n"
                                 "detach NetworkProvider/scripts/session_emit.etcs src=g1 dst=host\n");
        check(ETCS::run_script(lines, std::string(ETCS_ACE_ROOT) + "/modules/golf_wiring.etcs", wire), "the streams are wired with the page's scripts");
    }
    auto settle = [&]() {
        const uint64_t t = status(host).tick;
        for (int i = 0; i < 600 && status(g1).tick != t; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    auto run = [&](uint32_t ticks) { verb(host, "GolfMatch.Run", std::to_string(ticks)); settle(); };
    // A guest's line reaches the host on its seat: wait until it is taken.
    auto proposed = [&](const std::string& action, const std::string& args) {
        const uint64_t before = status(host).lines;
        verb(g1, action, args);
        for (int i = 0; i < 600 && status(host).lines == before; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    for (int i = 0; i < 300 && ball(host, "g1").slot < 0; ++i) run(2);
    run(30);
    const Ball h0 = ball(host, "host"), a0 = ball(host, "g1");
    std::printf("        (host %.2f,%.2f  g1 %.2f,%.2f)\n", h0.x, h0.z, a0.x, a0.z);
    check(h0.slot == 0 && a0.slot == 1 && std::fabs(a0.x - h0.x - 0.4f) < 0.01f && std::fabs(a0.z) < 0.01f,
          "two players, two balls side by side on the tee");
    check(verb(host, "GolfMatch.Here") == "g1\n" && verb(g1, "GolfMatch.Here") == "host\n",
          "...and each has the other here: the host the guest's seat, the guest the trace it follows");
    check(verb(g1, "GolfMatch.StartRound") == "refused" && verb(g1, "GolfMatch.Configure", "boosters 60") == "refused",
          "a guest sets nothing: the course, the time and the round are the host's");
    // A look is each player's own, and the lobby's: a colour of the palette
    // and a name, a line of the trace -- so every runtime shows it.
    {
        auto livery = [&](ETCS::Entity* g) { std::string c, n; std::istringstream in(verb(g, "GolfMatch.Livery")); in >> c >> n; return c + " " + n; };
        check(livery(host) == "0 host" && livery(g1) == "1 g1", "a player's look to begin with: its ball's own colour, its seat's label");
        proposed("GolfMatch.Recolour", "9");
        proposed("GolfMatch.Rename", "zed");
        run(4);
        check(livery(g1) == "9 zed" && verb(host, "GolfMatch.Scores").find(" zed ") != std::string::npos && verb(host, "GolfMatch.Scores").find(" g1 ") == std::string::npos,
              "the guest takes the tenth colour and the name zed: on the host's board too");
        check(ball(host, "zed").slot == 1 && ball(host, "g1").slot == 1, "a player is still found by its seat, and by its name too");
        check(verb(host, "GolfMatch.Rename", "zed") == "refused" && verb(host, "GolfMatch.Rename", "g1") == "refused" && verb(host, "GolfMatch.Recolour", "10") == "refused",
              "a name is one player's -- not another's, nor another's seat -- and a colour one of the ten");
        proposed("GolfMatch.Rename", "");
        run(4);
        check(livery(g1) == "9 g1", "no name is the seat's label again, the colour kept");
    }
    check(verb(host, "GolfMatch.Configure", "classic 30") == "ok" && verb(host, "GolfMatch.StartRound") == "ok", "the host starts a round on the classic");
    run(30);
    check(status(g1).phase == "playing" && status(g1).hole == 1, "...and the guest is on its first hole with it");
    check(verb(g1, "GolfMatch.Recolour", "2") == "refused" && verb(g1, "GolfMatch.Rename", "late") == "refused", "a look is not changed in a round");
    proposed("GolfMatch.Shoot", "0, 0, 1.2");
    run(250);
    const Ball a1 = ball(host, "g1");
    check(a1.strokes == 1 && a1.z > 3.0f && ball(g1, "g1").z == a1.z, "a guest's shot is a stroke on its card, the same on both runtimes");

    // -- 4. holed --------------------------------------------------------------
    std::cout << "\n-- 4. holed --\n";
    // The host teleports up the green in hops (a teleport goes ten at most),
    // then sits just past the lip and is nudged back into the cup.
    for (float z : { 10.0f, 20.0f, 30.0f, 40.0f })
    {
        verb(host, "GolfMatch.Give", "host teleport");
        verb(host, "GolfMatch.Use", "0, 0, " + std::to_string(z) + ", 0");
        run(20);
    }
    verb(host, "GolfMatch.Give", "host teleport");
    verb(host, "GolfMatch.Use", "0, 0, 44.6, 0");
    run(30);
    const Ball lip = ball(host, "host");
    std::printf("        (at the lip: %.2f %.2f %.2f)\n", lip.x, lip.y, lip.z);
    verb(host, "GolfMatch.Shoot", "0, 0, -0.3");
    run(80);
    const Ball in = ball(host, "host");
    std::printf("        (after the nudge: %.2f %.2f %.2f %s, strokes %d)\n", in.x, in.y, in.z, in.state.c_str(), in.strokes);
    check(std::fabs(lip.z - 44.6f) < 0.05f && in.state == "holed" && in.strokes == 1, "nudged back off the lip, it drops in: holed, in one stroke");
    run(60);
    check(ball(host, "host").y < -100.0f, "...and a moment later its ball leaves the course");

    // -- 5. the hole's time ----------------------------------------------------
    std::cout << "\n-- 5. the hole's time --\n";
    run(30 * 50);
    const Status s5 = status(host);
    const Ball a5 = ball(host, "g1");
    std::printf("        (%s, hole %d; g1 total %d)\n", s5.phase.c_str(), s5.hole, a5.total);
    check(a5.total == 9, "a ball still out when the time runs down is scored the most: the cap and one");
    run(5 * 50);
    const Ball h5 = ball(host, "host"), b5 = ball(host, "g1");
    check(status(host).hole == 2 && status(host).phase == "playing" && h5.strokes == 0 && b5.strokes == 0 && h5.total == 1,
          "then the next hole, everyone on its tee, the totals carried");

    // -- 6. the boosters -------------------------------------------------------
    std::cout << "\n-- 6. the boosters --\n";
    verb(host, "GolfMatch.Abandon");
    run(10);
    check(verb(host, "GolfMatch.Configure", "boosters 30") == "ok" && verb(host, "GolfMatch.StartRound") == "ok", "a round on the boosters, thirty seconds a hole");
    run(20);
    // Hops up the green by teleport, ten at most each.
    auto hop = [&](float x, float z) { verb(host, "GolfMatch.Give", "host teleport"); verb(host, "GolfMatch.Use", std::to_string(x) + ", 0, " + std::to_string(z) + ", 0"); run(20); };
    // The first hole: onto the pad at (0, 10) by a nudge along it.
    hop(0.0f, 8.0f);
    verb(host, "GolfMatch.Shoot", "0, 0, 0.5");
    run(25);
    const Ball pad = ball(host, "host");
    std::printf("        (off the pad: z %.2f, speed %.2f)\n", pad.z, pad.speed);
    check(pad.speed > 6.0f && pad.z > 9.0f, "a pad pushes a ball rolling onto it the way it points, far past its shot");
    // The second: a spring at (0, 25) before the wall across.
    run(30 * 50 + 5 * 50);
    hop(0.0f, 9.0f); hop(0.0f, 18.0f); hop(0.0f, 23.6f);
    verb(host, "GolfMatch.Shoot", "0, 0, 0.4");
    float high = 0.0f;
    for (int t = 0; t < 40; ++t) { run(2); high = std::max(high, ball(host, "host").y); }
    std::printf("        (hole %d; the highest over the spring: %.2f)\n", status(host).hole, high);
    check(status(host).hole == 2 && high > 1.0f, "a spring throws a ball rolling onto it up");
    // The third: the gap. A weak shot from the edge drops into it, lost.
    run(30 * 50 + 5 * 50);
    hop(0.0f, 9.0f); hop(4.0f, 19.0f); hop(4.0f, 25.0f);
    const Ball edge = ball(host, "host");
    verb(host, "GolfMatch.Shoot", "0, 0, 0.5");
    run(200);
    const Ball lost = ball(host, "host");
    std::printf("        (hole %d; from %.2f,%.2f to %.2f,%.2f,%.2f, strokes %d)\n", status(host).hole, edge.x, edge.z, lost.x, lost.y, lost.z, lost.strokes);
    check(status(host).hole == 3 && std::fabs(lost.z - edge.z) < 0.05f && lost.y > -0.5f && lost.strokes == 2,
          "short of the gap a ball is lost: back where it was struck, a stroke added");

    // -- 7. powers -------------------------------------------------------------
    std::cout << "\n-- 7. powers --\n";
    verb(host, "GolfMatch.Abandon");
    run(10);
    verb(host, "GolfMatch.Configure", "classic 60");
    verb(host, "GolfMatch.StartRound");
    run(30);
    const int before = slabs(host);
    verb(host, "GolfMatch.Give", "host bumper");
    verb(host, "GolfMatch.Use", "0, 0, 8, 90");
    run(2);
    check(slabs(host) == before + 1 && slabs(g1) == before + 1, "a bumper put down is a slab on the hole, on both runtimes");
    check(ball(host, "host").power == "-", "...and the power is used up");
    Ball hb = ball(host, "host");
    verb(host, "GolfMatch.Give", "host blackhole");
    verb(host, "GolfMatch.Use", std::to_string(hb.x + 3.0f) + ", 0, " + std::to_string(hb.z) + ", 0");
    run(40);
    const Ball pulled = ball(host, "host");
    std::printf("        (black hole: x %.2f -> %.2f)\n", hb.x, pulled.x);
    check(pulled.x > hb.x + 0.5f, "a black hole pulls a ball near it toward it");
    // A power at work is at work for half a minute, or until its hole is over:
    // a round begun again is a hole with nothing on it.
    auto fresh = [&]() { verb(host, "GolfMatch.Abandon"); run(10); verb(host, "GolfMatch.StartRound"); run(30); };
    fresh();
    const Ball w0 = ball(host, "g1");
    verb(host, "GolfMatch.Give", "host wind");
    verb(host, "GolfMatch.Use", "0, 0, 0, 90");
    run(60);
    const Ball w1 = ball(host, "g1");
    std::printf("        (wind toward +x: g1 x %.2f -> %.2f)\n", w0.x, w1.x);
    check(w1.x > w0.x + 0.3f, "a wind blows every ball the way its user looked -- another's too");
    run(250);
    const Ball w2 = ball(host, "g1");
    std::printf("        (...and six seconds on: x %.2f)\n", w2.x);
    check(w2.x > w1.x + 0.3f || w2.x > 7.0f, "...and goes on blowing: a power at work is at work for half a minute");
    fresh();
    const Ball t0 = ball(host, "host"), o0 = ball(host, "g1");
    verb(host, "GolfMatch.Give", "host teleport");
    verb(host, "GolfMatch.Use", std::to_string(t0.x) + ", 0, " + std::to_string(t0.z + 30.0f) + ", 0");
    run(20);
    const Ball t1 = ball(host, "host"), o1 = ball(host, "g1");
    std::printf("        (teleport: z %.2f -> %.2f)\n", t0.z, t1.z);
    check(std::fabs(t1.z - t0.z - 10.0f) < 0.1f && std::fabs(o1.z - o0.z) < 0.01f, "a teleport moves its user's ball, ten at most, and no one else's");
    verb(host, "GolfMatch.Give", "host tornado");
    verb(host, "GolfMatch.Use", std::to_string(t1.x - 2.0f) + ", 0, " + std::to_string(t1.z + 2.0f) + ", 0");
    float moved = 0.0f;
    for (int t = 0; t < 36; ++t)
    {
        run(10);
        const Ball now = ball(host, "host");
        moved = std::max(moved, std::fabs(now.x - t1.x) + std::fabs(now.z - t1.z));
    }
    std::printf("        (tornado: the host's ball went %.2f from where it was)\n", moved);
    check(moved > 1.0f, "a tornado near a ball swirls it, and its lightning throws it");
    fresh();

    // Two are carried: a spot's power goes to the spare when one is in hand,
    // in place of what the spare was, and comes to hand when that is used.
    verb(host, "GolfMatch.Give", "host pad");
    verb(host, "GolfMatch.Give", "host spring pickup");
    run(2);
    check(ball(host, "host").power == "pad" && ball(g1, "host").spare == "spring", "a power picked up with one in hand is the spare, on both runtimes");
    verb(host, "GolfMatch.Give", "host sand pickup");
    run(2);
    check(ball(host, "host").power == "pad" && ball(host, "host").spare == "sand", "...and the next one takes the spare's place");
    auto still = [&](const char* who) { for (int i = 0; i < 60 && ball(host, who).rest != "resting"; ++i) run(10); return ball(host, who); };
    Ball c0 = still("host");
    verb(host, "GolfMatch.Use", std::to_string(c0.x) + ", 0, " + std::to_string(c0.z + 3.0f) + ", 0");
    run(2);
    check(ball(host, "host").power == "sand" && ball(g1, "host").spare == "-", "the one in hand used, the spare comes to hand");

    // The cannon: the user's own ball, at rest, in an arc to the point.
    const float aim = c0.z > 24.0f ? c0.z - 8.0f : c0.z + 8.0f;
    verb(host, "GolfMatch.Give", "host cannon");
    verb(host, "GolfMatch.Use", "0, 0, " + std::to_string(aim) + ", 0");
    float peak = c0.y;
    for (int t = 0; t < 20; ++t) { run(5); peak = std::max(peak, ball(host, "host").y); }
    const Ball c1 = still("host");
    std::printf("        (cannon: from z %.2f at %.2f, up to y %.2f, down at %.2f,%.2f; strokes %d -> %d)\n", c0.z, aim, peak, c1.x, c1.z, c0.strokes, c1.strokes);
    check(peak > c0.y + 1.0f && std::fabs(c1.z - aim) < 2.5f && std::fabs(c1.x) < 2.5f && c1.strokes == c0.strokes, "a cannon fires its user's ball in an arc to the point, and it comes down dead there: no stroke");

    // The freeze: every other ball in the cold -- slowed, not stopped. g1's
    // shot, the same struck twice: once free, once frozen.
    still("g1");
    proposed("GolfMatch.Shoot", "1.5, 0, 0");
    run(5);
    const float free0 = ball(host, "g1").speed;
    run(40);
    const float free1 = ball(host, "g1").speed;
    still("g1");
    verb(host, "GolfMatch.Give", "host freeze");
    verb(host, "GolfMatch.Use", std::to_string(c1.x) + ", 0, " + std::to_string(c1.z) + ", 0");
    proposed("GolfMatch.Shoot", "-1.5, 0, 0");
    run(5);
    const float cold0 = ball(host, "g1").speed;
    run(40);
    const float cold1 = ball(host, "g1").speed;
    std::printf("        (g1's shot: free %.2f -> %.2f u/s in 40 ticks; frozen %.2f -> %.2f)\n", free0, free1, cold0, cold1);
    check(free0 > 2.0f && cold0 > 1.0f && cold1 > 0.05f && cold1 / cold0 < 0.75f * free1 / free0 && ball(g1, "g1").speed == cold1,
          "a freeze slows every other ball -- it still plays, and its shot carries less far");

    // Smoke: a cloud put down, thick and then dissipating -- in the match as
    // an effect, on both runtimes.
    verb(host, "GolfMatch.Give", "host smoke");
    verb(host, "GolfMatch.Use", std::to_string(c1.x) + ", 0, " + std::to_string(c1.z + 4.0f) + ", 0");
    run(2);
    check(verb(g1, "GolfMatch.Note") == "host put down a smoke" && ball(host, "host").power == "-", "a smoke cloud is put down where it is pointed");

    // The magnet: the cup pulls its user's ball from as far as ten off.
    fresh();
    for (int k = 0; k < 6; ++k)
    {
        const Ball at = still("host");
        if (std::fabs(at.z - 37.0f) < 0.5f && std::fabs(at.x) < 0.5f) break;
        verb(host, "GolfMatch.Give", "host teleport");
        verb(host, "GolfMatch.Use", "0, 0, 37, 0");
        run(10);
    }
    const Ball m0 = still("host");
    verb(host, "GolfMatch.Give", "host magnet");
    verb(host, "GolfMatch.Use", "0, 0, 0, 0");
    for (int t = 0; t < 60 && ball(host, "host").state != "holed"; ++t) run(10);
    const Ball m1 = ball(host, "host");
    std::printf("        (magnet: the cup at 0,44; the ball from %.2f,%.2f to %.2f,%.2f, %s)\n", m0.x, m0.z, m1.x, m1.z, m1.state.c_str());
    check(m1.state == "holed", "a magnet draws its user's ball to the cup, slowly enough to drop in");

    // -- 8. the round ----------------------------------------------------------
    std::cout << "\n-- 8. the round --\n";
    run(60 * 50 * 3 + 20 * 50);   // the rest of the classic, at a minute a hole
    const Status s8 = status(host);
    const std::string board = verb(host, "GolfMatch.Scores");
    std::printf("        (%s; board %s)\n", s8.phase.c_str(), board.c_str());
    check(s8.phase == "lobby" && board.find("host") != std::string::npos, "every hole played out, then the results, then the lobby, the board kept");
    check(verb(host, "GolfMatch.Configure", "random 120") == "ok", "a course from a seed");
    run(10);
    check(verb(host, "GolfMatch.Layout") == verb(g1, "GolfMatch.Layout"), "...the same course on both runtimes");
    verb(host, "GolfMatch.Reroll");
    run(10);
    check(verb(host, "GolfMatch.Layout") == verb(g1, "GolfMatch.Layout") && status(host).layout != 0, "...and a new seed, still the same on both");

    // -- 9. the same match -----------------------------------------------------
    std::cout << "\n-- 9. the same match --\n";
    run(300);
    const std::string hh = verb(host, "GolfMatch.Hash"), gh = verb(g1, "GolfMatch.Hash");
    std::printf("        (host %s, guest %s)\n", hh.c_str(), gh.c_str());
    check(hh == gh, "the guest at the host's tick holds the host's match, to the bit");
    check(verb(host, "GolfMatch.Snapshot") == verb(g1, "GolfMatch.Snapshot") && verb(g1, "GolfMatch.Snapshot").compare(0, 2, "0 ") != 0,
          "...and checked itself where the host's trace said its hash");
    check(verb(g1, "GolfMatch.Note").find("differs") == std::string::npos, "...and never found itself out of step");

    // The streams wound up as a closing script winds up what it detached:
    // each told to stop, then joined.
    auto& detached = ETCS::DetachedRegistry::getInstance();
    for (auto& [id, script] : detached.list()) detached.interrupt(id);
    for (int i = 0; i < 2500 && !detached.all_finished(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(detached.all_finished(), "the two streams end when told to");
    detached.join_all();
    check(ball(host, "g1").slot < 0 && verb(host, "GolfMatch.Here").empty(),
          "the guest's seat ending is its leave: its ball off the course, nobody here");
    for (ETCS::Entity* g : game) verb(g, "GolfMatch.Delete");
    ETCS::PendingUnloadRegistry::getInstance().join_all();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
