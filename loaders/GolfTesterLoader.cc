// GolfTesterLoader.cc
//
// A COURSE, PLAYED WITHOUT A WINDOW. The course script (GolfProvider/scripts/
// golf_course.etcs) builds its three holes under a world and hands their
// pieces to a GolfGame; this plays the first the way a pointer would --
// through the game's Press/Drag/Release verbs -- and steps the world with its
// driver, the way a headless run does (Scene3D::Run). What is held to account:
//
//   1. THE COURSE STANDS. Built by the script, stepped, the ball comes to rest
//      on the tee: the world's field pulls it down, the green holds it up, and
//      its shadow lies on the green straight under it.
//   2. THE PULL. A press on the ball (a camera ray through the middle of the
//      frame, where the game put the ball) starts a pull; dragging down pulls
//      the shot away from the camera and up -- the view plane's slingshot --
//      as long as the drag, to the cap. The arrow's shadow lies flat on the
//      ground under it. Cancelled, the pull is put down and nothing is shot.
//   3. THE SHOT. Letting go is the ball's own Impulse along the pull: the ball
//      moves that way, and the card counts a stroke. In the air its shadow
//      stays on the green under it.
//   4. THE CAMERA. A drag off the ball turns the camera about it and leaves
//      the ball alone; the wheel brings it closer. With nothing moving, the
//      turned eye is still drawn again (and an unturned one is not).
//   5. HOLED. A ball sent at the cup at a putting pace drops through the gap,
//      fit takes it into the cup's space, and it rests on the cup's floor; the
//      game, advanced at the frame edge, sets it on the next hole's tee.
//   6. THE COURSE AND ITS LEVELS. Hole 2 starts on a terrace: a ball rolled
//      off it runs down the slope and comes to rest a level down, where a
//      putt holes it. Hole 3 climbs: a soft putt at the slope rolls back, a
//      firm one rests on the terrace a level up; a ball dropped into the sand
//      trap rests half a unit down, its shadow on the sand. Holed, the round
//      is done and the first hole comes round again; a hole can be gone to by
//      its number, and N goes on to the next.
//   7. AT SPEED. Stepped a tenth of a second at a time, a lob comes down on
//      the green and a drive is turned back by a bumper thinner than its
//      step: a contact is found on the way, not where the step ends. A ball
//      pushed past the course's speed of light moves at it.
//   8. HOLED BY BEING LOOKED AT. The same putt with no driver: the camera's
//      pictures step the world, as a frame does. A picture is drawn under a
//      hold and only notes its look; the step, and fit's move, come after it.
//
//   ./Run_GolfTesterLoader
//
// No display, no GPU.

#include "../ETCS.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

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

// The world-frame centre of a Causal node.
static void where(Causal_* c, float out[3])
{
    Fixed bx, by, bz;
    c->Basis(bx, by, bz);
    std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
    const OrderVector& o = c->Order4();
    out[0] = (bx + o.x).ToFloat(); out[1] = (by + o.y).ToFloat(); out[2] = (bz + o.z).ToFloat();
}
static float speed(Causal_* c)
{
    std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
    Fixed vx, vy, vz;
    c->Order4().Velocity(Fixed::One(), vx, vy, vz);
    return Fixed::Length(vx, vy, vz).ToFloat();
}
static void velocity(Causal_* c, float v[3])
{
    std::lock_guard<std::recursive_mutex> lk(c->TreeMutex());
    Fixed vx, vy, vz;
    c->Order4().Velocity(Fixed::One(), vx, vy, vz);
    v[0] = vx.ToFloat(); v[1] = vy.ToFloat(); v[2] = vz.ToFloat();
}

int main(int, char**)
{
    WIRE_CONTEXT();
    std::cout << "=== A course, without a window ===\n";

    // The session's half: a world, a camera on it, a game -- what a page or a
    // window script makes before it runs the course.
    ETCS::Entity* world = ETCS::spawn_entity("RenderProvider", "Scene3D", env, loader);
    ETCS::Entity* eye   = ETCS::spawn_entity("RenderProvider", "Camera3D", env, loader);
    ETCS::Entity* game  = ETCS::spawn_entity("GolfProvider", "GolfGame", env, loader);
    if (!world || !eye || !game) { std::printf("FAILED (could not spawn the session)\n"); return 1; }
    verb(world, "Scene3D.Create", "1, 1, 1");
    verb(world, "Scene3D.SetVisible", "0");
    verb(eye, "Camera3D.Create", "1024, 768");
    verb(eye, "Camera3D.SetLens", "55, 0.05, 300");
    verb(eye, "Camera3D.SetScene", std::to_string(world->getRID()));

    // The course, run as a script with the session's names handed in.
    ETCS::RID shadow_rid = 0, under_rid = 0;
    {
        ETCS::ExecutionContext course(&root, &ctx);
        course.names["anchor"] = ETCS::NameBinding{ world->getRID(), "RenderProvider", "Scene3D" };
        course.names["eye"]    = ETCS::NameBinding{ eye->getRID(), "RenderProvider", "Camera3D" };
        course.names["game"]   = ETCS::NameBinding{ game->getRID(), "GolfProvider", "GolfGame" };
        const std::string path = std::string(ETCS_ACE_ROOT) + "/modules/GolfProvider/scripts/golf_course.etcs";
        std::ifstream in(path);
        check(in.is_open() && ETCS::run_script(in, path, course), "the course script runs");
        if (course.names.count("arrow_shadow")) shadow_rid = course.names["arrow_shadow"].rid;
        if (course.names.count("ball_shadow")) under_rid = course.names["ball_shadow"].rid;
    }

    auto* wc  = static_cast<Causal_*>(world->getInterfacePointer(ETCS::Buffer("Causal")));
    auto* cam = static_cast<Camera_*>(eye->getInterfacePointer(ETCS::Buffer("Camera")));
    auto* anim = static_cast<Animated_*>(game->getInterfacePointer(ETCS::Buffer("Animated")));
    // The ball and the cups, found as what they are: the free solid sphere,
    // and the containers with a space of their own -- each hole's, west to
    // east, which is the order they are played in.
    Causal_* ball = nullptr; Causal_* cup = nullptr; Causal_* shadow = nullptr; Causal_* under = nullptr;
    std::vector<Causal_*> cups;
    {
        std::vector<ETCS::Entity::ChildRef> kids;
        world->getTypedChildRefs(kids);
        for (auto& [tag, rid] : kids)
        {
            ETCS::Entity* k = world->getTypedChild(*tag, rid);
            auto* c = k ? static_cast<Causal_*>(k->getInterfacePointer(ETCS::Buffer("Causal"))) : nullptr;
            if (!c) continue;
            if (rid == shadow_rid) shadow = c;
            if (rid == under_rid) under = c;
            if (c->Solid().shape == CausalSolid::Sphere) ball = c;
            if (c->Space().IsPositive()) cups.push_back(c);
        }
    }
    std::sort(cups.begin(), cups.end(), [](Causal_* a, Causal_* b) { float p[3], q[3]; where(a, p); where(b, q); return p[0] < q[0]; });
    if (!cups.empty()) cup = cups[0];
    check(wc && cam && anim && ball && cups.size() == 3, "the course has a ball and three holes, a cup to each");
    if (!(wc && cam && anim && ball && cups.size() == 3)) { std::printf("FAILED\n"); return 1; }
    check(verb(game, "GolfGame.Card") == "1 3 0 0", "...the first in play, the card clear");
    auto step = [&](int ticks) { for (int i = 0; i < ticks; ++i) { verb(world, "Scene3D.Run", "1, 16"); anim->Advance(); } };

    // -- 1. the course stands ----------------------------------------------------
    std::cout << "\n-- 1. the course stands --\n";
    step(120);
    float b[3];
    where(ball, b);
    check(std::fabs(b[1] - 0.15f) < 0.003f && speed(ball) == 0.0f, "the ball comes to rest on the tee: the field down, the green up");
    check(!wc->Moving(), "...and the world says nothing is moving");
    check(under != nullptr, "the course makes the ball a shadow");
    if (under)
    {
        float u[3];
        where(under, u);
        check(std::fabs(u[0] - b[0]) < 1e-3f && std::fabs(u[2] - b[2]) < 1e-3f && u[1] > 0.02f && u[1] < 0.05f,
              "...lying on the green straight under the ball");
    }

    // -- 2. the pull ---------------------------------------------------------------
    std::cout << "\n-- 2. the pull --\n";
    const ViewFrustum v0 = cam->GetView();
    check(std::fabs(v0.look_at.z - b[2]) < 1e-3f && v0.position.z < b[2], "the camera is behind the ball, looking at it");
    verb(game, "GolfGame.Press", "512, 384");
    verb(game, "GolfGame.Drag", "512, 440");
    float px, py, pz, plen; char mode[16] = {};
    std::sscanf(verb(game, "GolfGame.Pull").c_str(), "%f %f %f %f %15s", &px, &py, &pz, &plen, mode);
    check(std::string(mode) == "pulling", "a press on the ball starts a pull");
    check(pz > 0.0f && py > 0.0f && std::fabs(px) < 1e-3f, "dragging down pulls the shot away from the camera, and up: the view plane's loft");
    const float short_len = plen;
    verb(game, "GolfGame.Drag", "512, 760");
    std::sscanf(verb(game, "GolfGame.Pull").c_str(), "%f %f %f %f %15s", &px, &py, &pz, &plen, mode);
    check(plen > short_len && std::fabs(plen - 3.0f) < 1e-3f, "a longer drag is a longer pull, to the cap");
    // The arrow's shadow: the arrow laid flat on the ground under it. Its
    // length (the mesh's +y) runs along the pull's ground track, its depth
    // (+z) is vertical, and it sits where the ball stands, not where the
    // arrow rises to.
    {
        check(shadow != nullptr, "the course makes the arrow a shadow");
        if (shadow)
        {
            float at[3];
            where(shadow, at);
            Fixed lx = Fixed::Zero(), ly = Fixed::One(), lz = Fixed::Zero();
            Fixed dx = Fixed::Zero(), dy = Fixed::Zero(), dz = Fixed::One();
            {
                std::lock_guard<std::recursive_mutex> lk(shadow->TreeMutex());
                shadow->Order4().RotateVector(lx, ly, lz);
                shadow->Order4().RotateVector(dx, dy, dz);
            }
            const float run = std::sqrt(px * px + pz * pz);
            check(std::fabs(lx.ToFloat() - px / run) < 1e-3f && std::fabs(ly.ToFloat()) < 1e-3f && std::fabs(lz.ToFloat() - pz / run) < 1e-3f,
                  "the shadow lies along the pull's track over the ground");
            check(std::fabs(std::fabs(dy.ToFloat()) - 1.0f) < 1e-3f, "...flat: its depth is the vertical");
            check(at[1] > 0.0f && at[1] < 0.05f && std::fabs(at[2] - (b[2] + (0.15f + plen * 0.5f) * pz / plen)) < 1e-2f,
                  "...on the ground the ball stands on, straight under the arrow's middle");
        }
    }
    // Put down, not shot: what the right button does while the left holds a
    // pull. The arrow goes, nothing is struck, and letting go finds nothing
    // in hand.
    {
        verb(game, "GolfGame.Cancel");
        std::sscanf(verb(game, "GolfGame.Pull").c_str(), "%f %f %f %f %15s", &px, &py, &pz, &plen, mode);
        const bool down = std::string(mode) == "idle" && plen == 0.0f;
        verb(game, "GolfGame.Drag", "512, 600");
        verb(game, "GolfGame.Release", "512, 600");
        step(5);
        float at[3];
        where(ball, at);
        check(down && verb(game, "GolfGame.Strokes") == "0" && speed(ball) == 0.0f && std::fabs(at[2] - b[2]) < 1e-6f,
              "a pull cancelled is put down: no arrow, no stroke, and letting go after it shoots nothing");
        verb(game, "GolfGame.Press", "512, 384");
        verb(game, "GolfGame.Drag", "512, 440");
    }

    // -- 3. the shot -------------------------------------------------------------
    std::cout << "\n-- 3. the shot --\n";
    std::sscanf(verb(game, "GolfGame.Pull").c_str(), "%f %f %f %f %15s", &px, &py, &pz, &plen, mode);
    verb(game, "GolfGame.Release", "512, 440");
    float vel[3];
    velocity(ball, vel);
    const float vl = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
    const float cosang = (vel[0] * px + vel[1] * py + vel[2] * pz) / (vl * plen);
    check(vl > 0.1f && cosang > 0.999f, "letting go: the ball moves along the pull");
    check(std::fabs(vl - 12.0f * plen / 3.0f) < 0.05f, "...at the speed the pull's length gives");
    check(verb(game, "GolfGame.Strokes") == "1", "...and the card counts a stroke");
    step(1);
    check(wc->Moving(), "...and the world says something is moving: a frame edge keeps drawing");
    if (under)
    {
        // The ball in the air: its shadow stays on the green, under it.
        float top[3] = { 0, 0, 0 }, low[3] = { 0, 0, 0 };
        for (int i = 0; i < 40; ++i)
        {
            step(1);
            float at[3];
            where(ball, at);
            if (at[1] > top[1]) { top[0] = at[0]; top[1] = at[1]; top[2] = at[2]; where(under, low); }
        }
        std::printf("        (the ball up to y %.2f at z %.2f; its shadow there at y %.3f, z %.2f)\n", top[1], top[2], low[1], low[2]);
        check(top[1] > 0.2f && low[1] > 0.02f && low[1] < 0.05f && std::fabs(low[0] - top[0]) < 1e-3f && std::fabs(low[2] - top[2]) < 1e-3f,
              "the ball in the air has its shadow on the green straight under it");
    }
    step(600);

    // -- 4. the camera -------------------------------------------------------------
    std::cout << "\n-- 4. the camera --\n";
    where(ball, b);
    const ViewFrustum v1 = cam->GetView();
    check(std::fabs(v1.look_at.x - b[0]) < 1e-3f && std::fabs(v1.look_at.z - b[2]) < 1e-3f, "the camera followed the ball");
    verb(game, "GolfGame.Press", "50, 50");
    verb(game, "GolfGame.Drag", "150, 50");
    std::sscanf(verb(game, "GolfGame.Pull").c_str(), "%f %f %f %f %15s", &px, &py, &pz, &plen, mode);
    const ViewFrustum v2 = cam->GetView();
    verb(game, "GolfGame.Release", "150, 50");
    float b2[3];
    where(ball, b2);
    check(std::string(mode) == "orbiting" && std::fabs(v2.position.x - v1.position.x) > 0.1f
          && b2[0] == b[0] && b2[2] == b[2], "a drag off the ball turns the camera about it, and leaves the ball alone");
    auto dist = [&](const ViewFrustum& v) {
        const float dx = v.position.x - v.look_at.x, dy = v.position.y - v.look_at.y, dz = v.position.z - v.look_at.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    verb(game, "GolfGame.Wheel", "3");
    check(dist(cam->GetView()) < dist(v2) - 0.5f, "the wheel brings it in");
    // On a still scene a turned eye is still a new picture: the camera
    // re-renders for it, and not for nothing.
    {
        ETCS::Entity* stage = ETCS::spawn_entity("RenderProvider", "CompositeDrawable2D", env, loader);
        verb(stage, "CompositeDrawable2D.Create", "1024, 768");
        const std::string target = std::to_string(stage->getRID());
        auto draw = [&]() { verb(eye, "Camera3D.Draw", target); return verb(eye, "Camera3D.Renders"); };
        draw();
        const std::string settled = draw();
        verb(game, "GolfGame.Press", "50, 50");
        verb(game, "GolfGame.Drag", "120, 50");
        verb(game, "GolfGame.Release", "120, 50");
        const std::string turned = draw();
        check(!wc->Moving() && !settled.empty() && draw() == turned && turned != settled,
              "a still scene: the camera draws again for a turned eye, and only then");
        verb(stage, "CompositeDrawable2D.Delete");
    }

    // -- 5. holed --------------------------------------------------------------------
    std::cout << "\n-- 5. holed --\n";
    // Three short of the cup, past the ridge, at a putting pace: rolling
    // friction takes ~0.98 m/s^2, so 4 J (2.8 m/s) arrives at about 1.2.
    ETCS::Entity* be = static_cast<ETCS::Entity*>(ball);
    verb(be, "Scene3D.Halt");
    verb(be, "Scene3D.SetPosition", "0, 0.15, 19");
    step(60);
    // What the world is made of, as an export counts it: the same walk and the
    // same shape lookup a picture makes (Scene3D::collectSubtree).
    const std::string shapes = "/tmp/etcs_golf_tester_" + std::to_string(::getpid()) + ".stl";
    const std::string on_green = verb(world, "Scene3D.Export", shapes);
    verb(be, "Scene3D.Impulse", "0, 0, 1, 4");
    bool holed = false;
    for (int i = 0; i < 900 && !holed; ++i)
    {
        step(1);
        holed = static_cast<ETCS::Entity*>(ball)->getParent() == static_cast<ETCS::Entity*>(cup);
    }
    check(holed, "a putt at the cup drops through the gap, and fit takes it into the cup");
    step(120);
    where(ball, b);
    check(holed && speed(ball) == 0.0f && b[1] < -0.5f, "...and rests on the cup's own floor");
    // The cup holds no meshes, and "one up" from the ball is now the cup: the
    // ball is drawn by the RID it was given, not by where it has got to.
    const std::string in_cup = verb(world, "Scene3D.Export", shapes);
    ::unlink(shapes.c_str());
    check(holed && !on_green.empty() && on_green.find("refused") == std::string::npos && in_cup == on_green,
          "...still the sphere it was on the green (the mesh is anchored by RID inside the run)");
    // A store hands every value back after the lines that set them (etcs_restore_values):
    // the address as SetMesh wrote it on the green, to a ball that is now in the cup.
    {
        std::string kept;
        const bool had = be->valueOf(ETCS::Buffer("mesh"), kept);
        check(had && be->restoreValue(ETCS::Buffer("mesh"), kept)
              && verb(world, "Scene3D.Export", shapes) == on_green,
              "...and still when its stored value is handed back (the line's RID stands)");
        ::unlink(shapes.c_str());
    }
    // The game sets it back once the card has been read: 2.5 s of frames, as
    // the frame edge would visit (the base measures the time between visits).
    for (int i = 0; i < 80 && static_cast<ETCS::Entity*>(ball)->getParent() != world; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        anim->Advance();
    }
    where(ball, b);
    std::printf("        (the card: %s; the ball at %.2f %.2f %.2f)\n", verb(game, "GolfGame.Card").c_str(), b[0], b[1], b[2]);
    check(static_cast<ETCS::Entity*>(ball)->getParent() == world && std::fabs(b[0] - 14.0f) < 0.01f && std::fabs(b[2]) < 0.01f && b[1] > 1.5f,
          "the game sets a holed ball on the next hole's tee");
    check(verb(game, "GolfGame.Card") == "2 3 0 1", "...the card on hole 2, the first hole's stroke kept for the round");

    // -- 6. the course and its levels ---------------------------------------------------
    std::cout << "\n-- 6. the course and its levels --\n";
    // A putt from three short of a cup, as in 5, and the frames the game reads the card over.
    auto hole_out = [&](Causal_* into, const char* from) {
        verb(be, "Scene3D.Halt");
        verb(be, "Scene3D.SetPosition", from);
        step(60);
        verb(be, "Scene3D.Impulse", "0, 0, 1, 4");
        bool in = false;
        for (int i = 0; i < 900 && !in; ++i) { step(1); in = be->getParent() == static_cast<ETCS::Entity*>(into); }
        return in;
    };
    auto read_card = [&]() {
        for (int i = 0; i < 80 && be->getParent() != world; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); anim->Advance(); }
    };
    auto rest = [&](int most) { for (int i = 0; i < most; ++i) { step(10); if (speed(ball) == 0.0f) break; } where(ball, b); };
    {
        // Hole 2. On its terrace; rolled off it, down the slope to the green.
        rest(30);
        check(std::fabs(b[1] - 1.65f) < 0.003f && speed(ball) == 0.0f, "hole 2's tee is on a terrace: the ball rests a unit and a half up");
        verb(be, "Scene3D.Impulse", "0, 0, 1, 9");
        float lowest = b[1], furthest = b[2];
        for (int i = 0; i < 150; ++i) { step(10); where(ball, b); lowest = std::min(lowest, b[1]); furthest = std::max(furthest, b[2]); if (speed(ball) == 0.0f) break; }
        std::printf("        (rolled off the terrace: down to y %.2f, as far as z %.2f; at rest at %.2f %.2f %.2f)\n", lowest, furthest, b[0], b[1], b[2]);
        check(speed(ball) == 0.0f && std::fabs(b[1] - 0.15f) < 0.003f && b[2] > 14.0f && b[2] < 26.0f,
              "...and rolled off it, it runs down the slope and comes to rest on the green, a level down");
        const bool in2 = hole_out(cups[1], "14, 0.15, 19");
        check(in2, "a putt on that green is holed in hole 2's cup");
        read_card();
        where(ball, b);
        std::printf("        (the card: %s; the ball at %.2f %.2f %.2f)\n", verb(game, "GolfGame.Card").c_str(), b[0], b[1], b[2]);
        check(be->getParent() == world && std::fabs(b[0] - 28.0f) < 0.01f && std::fabs(b[2]) < 0.01f && verb(game, "GolfGame.Card").compare(0, 5, "3 3 0") == 0,
              "...and the game goes on to hole 3's tee");

        // Hole 3. A soft putt at the slope rolls back; a firm one stays up.
        verb(be, "Scene3D.Halt");
        verb(be, "Scene3D.SetPosition", "26, 0.15, 7");
        step(30);
        verb(be, "Scene3D.Impulse", "0, 0, 1, 4");
        float highest = 0.0f;
        for (int i = 0; i < 150; ++i) { step(10); where(ball, b); highest = std::max(highest, b[1]); if (speed(ball) == 0.0f) break; }
        std::printf("        (a soft putt at the slope: up to y %.2f, at rest at z %.2f, y %.2f)\n", highest, b[2], b[1]);
        check(speed(ball) == 0.0f && highest > 0.2f && highest < 1.0f && std::fabs(b[1] - 0.15f) < 0.003f && b[2] < 8.0f,
              "hole 3 climbs: a soft putt goes part way up the slope and rolls back to the green");
        verb(be, "Scene3D.Halt");
        verb(be, "Scene3D.SetPosition", "26, 0.15, 7");
        step(30);
        verb(be, "Scene3D.Impulse", "0, 0, 1, 20");
        rest(150);
        std::printf("        (a firm one: at rest at z %.2f, y %.2f)\n", b[2], b[1]);
        check(speed(ball) == 0.0f && std::fabs(b[1] - 1.15f) < 0.003f && b[2] > 14.0f && b[2] < 26.0f,
              "...a firm one tops it and rests on the terrace, a level up");
        // The trap, in the straight line from the tee.
        verb(be, "Scene3D.Halt");
        verb(be, "Scene3D.SetPosition", "28, 0.4, 5");
        rest(60);
        float u[3] = { 0, 0, 0 };
        if (under) where(under, u);
        std::printf("        (dropped into the trap: at rest at y %.2f, its shadow at y %.3f)\n", b[1], u[1]);
        check(speed(ball) == 0.0f && std::fabs(b[1] + 0.35f) < 0.003f && u[1] > -0.5f && u[1] < -0.44f,
              "...and the sand trap before the slope is half a unit down: a ball in it rests there, its shadow on the sand");
        const std::string before = verb(game, "GolfGame.Card");
        const bool in3 = hole_out(cups[2], "28, 1.15, 19");
        check(in3, "a putt on the terrace is holed in hole 3's cup");
        read_card();
        where(ball, b);
        std::printf("        (the card before: %s, after: %s; the ball at %.2f %.2f %.2f)\n", before.c_str(), verb(game, "GolfGame.Card").c_str(), b[0], b[1], b[2]);
        check(be->getParent() == world && std::fabs(b[0]) < 0.01f && std::fabs(b[2]) < 0.01f && verb(game, "GolfGame.Card") == "1 3 0 0",
              "the last hole holed, the round is done: the first hole again, the card clear");
        // By number, and on with N.
        verb(game, "GolfGame.PlayHole", "2");
        where(ball, b);
        const bool second = std::fabs(b[0] - 14.0f) < 0.01f && verb(game, "GolfGame.Card") == "2 3 0 0";
        verb(game, "GolfGame.Key", "78, 1");
        where(ball, b);
        check(second && std::fabs(b[0] - 28.0f) < 0.01f && verb(game, "GolfGame.Card") == "3 3 0 0", "a hole is gone to by its number, and N goes on to the next");
        verb(game, "GolfGame.Reset");
        step(60);
    }

    // -- 7. at speed ------------------------------------------------------------------
    std::cout << "\n-- 7. at speed --\n";
    {
        // Long frames, a tenth of a second each: what a slow machine gives.
        // A lob comes down at 0.57 a step, on a green 0.5 thick; a drive is
        // 1.2 a step, at a bumper 0.3 thick.
        auto slow = [&](int ticks) { for (int i = 0; i < ticks; ++i) { verb(world, "Scene3D.Run", "1, 100"); anim->Advance(); } };
        verb(be, "Scene3D.Halt");
        verb(be, "Scene3D.SetPosition", "2, 0.15, 2");
        slow(3);
        verb(be, "Scene3D.Impulse", "0, 0.7071, 0.7071, 32");
        float lowest = 9.0f, highest = 0.0f;
        for (int i = 0; i < 80; ++i) { slow(1); where(ball, b); lowest = std::min(lowest, b[1]); highest = std::max(highest, b[1]); if (speed(ball) == 0.0f && i > 5) break; }
        std::printf("        (a lob on long frames: up to y %.2f, never below y %.3f; at rest at %.2f %.3f %.2f)\n", highest, lowest, b[0], b[1], b[2]);
        check(highest > 1.0f && lowest > 0.148f && speed(ball) == 0.0f && std::fabs(b[1] - 0.15f) < 0.003f && be->getParent() == world,
              "a lob stepped a tenth of a second at a time comes down on the green, not through it");
        verb(be, "Scene3D.Halt");
        verb(be, "Scene3D.SetPosition", "2, 0.15, 20");
        slow(3);
        verb(be, "Scene3D.Impulse", "0, 0, 1, 72");
        float furthest = 0.0f;
        for (int i = 0; i < 120; ++i) { slow(1); where(ball, b); furthest = std::max(furthest, b[2]); if (speed(ball) == 0.0f && i > 5) break; }
        std::printf("        (a drive at the far bumper: as far as z %.3f; at rest at %.2f %.3f %.2f)\n", furthest, b[0], b[1], b[2]);
        check(furthest < 26.0f && furthest > 25.5f && speed(ball) == 0.0f && std::fabs(b[1] - 0.15f) < 0.003f && b[2] < 25.9f,
              "a drive at a bumper thinner than its step is turned back by it, and stays on the green");
        // The course's speed of light: 16 a second.
        verb(be, "Scene3D.Halt");
        verb(be, "Scene3D.SetPosition", "2, 0.15, 2");
        slow(3);
        verb(be, "Scene3D.Impulse", "0, 1, 0, 800");   // 40 a second, asked for
        verb(world, "Scene3D.Run", "1, 16");
        std::printf("        (40 a second asked of the ball: %.3f)\n", speed(ball));
        check(speed(ball) <= 16.0f && speed(ball) > 15.5f, "nothing on the course outruns its light: a ball pushed past 16 a second moves at 16");
        verb(be, "Scene3D.Halt");
        verb(game, "GolfGame.Reset");
        step(60);
    }

    // -- 8. holed by being looked at ---------------------------------------------------
    std::cout << "\n-- 8. holed by being looked at --\n";
    verb(be, "Scene3D.Halt");
    verb(be, "Scene3D.SetPosition", "0, 0.15, 19");
    step(60);
    verb(eye, "Camera3D.Render");   // the first look: the clocks start, nothing is owed
    verb(be, "Scene3D.Impulse", "0, 0, 1, 4");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // The picture alone, as a walk asks for it (the interface, not the verb):
    // drawn under the camera's hold on the scene, where a move cannot be made.
    const uint64_t drawn = wc->CausalHash();
    cam->Render();
    check(wc->CausalHash() == drawn, "a picture reads: the rows after it are the rows it drew");
    verb(eye, "Camera3D.Render");
    check(wc->CausalHash() != drawn, "...and its look is stepped once the picture is done");
    holed = false;
    for (int i = 0; i < 1500 && !holed; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        verb(eye, "Camera3D.Render");
        holed = be->getParent() == static_cast<ETCS::Entity*>(cup);
    }
    check(holed, "a putt stepped by looks alone is holed: fit's move is made outside the picture's hold");

    verb(game, "GolfGame.Delete");
    ETCS::PendingUnloadRegistry::getInstance().join_all();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
