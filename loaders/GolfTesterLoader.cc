// GolfTesterLoader.cc
//
// ONE HOLE, PLAYED WITHOUT A WINDOW. The course script (GolfProvider/scripts/
// golf_course.etcs) builds the hole under a world and hands its pieces to a
// GolfGame; this plays it the way a pointer would -- through the game's
// Press/Drag/Release verbs -- and steps the world with its driver, the way a
// headless run does (Scene3D::Run). What is held to account:
//
//   1. THE COURSE STANDS. Built by the script, stepped, the ball comes to rest
//      on the tee: the world's field pulls it down, the green holds it up.
//   2. THE PULL. A press on the ball (a camera ray through the middle of the
//      frame, where the game put the ball) starts a pull; dragging down pulls
//      the shot away from the camera and up -- the view plane's slingshot --
//      as long as the drag, to the cap.
//   3. THE SHOT. Letting go is the ball's own Impulse along the pull: the ball
//      moves that way, and the card counts a stroke.
//   4. THE CAMERA. A drag off the ball turns the camera about it and leaves
//      the ball alone; the wheel brings it closer. With nothing moving, the
//      turned eye is still drawn again (and an unturned one is not).
//   5. HOLED. A ball sent at the cup at a putting pace drops through the gap,
//      fit takes it into the cup's space, and it rests on the cup's floor; the
//      game, advanced at the frame edge, sets it back on the tee.
//
//   ./Run_GolfTesterLoader
//
// No display, no GPU.

#include "../ETCS.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
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
    std::cout << "=== One hole, without a window ===\n";

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
    {
        ETCS::ExecutionContext course(&root, &ctx);
        course.names["anchor"] = ETCS::NameBinding{ world->getRID(), "RenderProvider", "Scene3D" };
        course.names["eye"]    = ETCS::NameBinding{ eye->getRID(), "RenderProvider", "Camera3D" };
        course.names["game"]   = ETCS::NameBinding{ game->getRID(), "GolfProvider", "GolfGame" };
        const std::string path = std::string(ETCS_ACE_ROOT) + "/modules/GolfProvider/scripts/golf_course.etcs";
        std::ifstream in(path);
        check(in.is_open() && ETCS::run_script(in, path, course), "the course script runs");
    }

    auto* wc  = static_cast<Causal_*>(world->getInterfacePointer(ETCS::Buffer("Causal")));
    auto* cam = static_cast<Camera_*>(eye->getInterfacePointer(ETCS::Buffer("Camera")));
    auto* anim = static_cast<Animated_*>(game->getInterfacePointer(ETCS::Buffer("Animated")));
    // The ball and the cup, found as what they are: the free solid sphere, and
    // the container with a space of its own.
    Causal_* ball = nullptr; Causal_* cup = nullptr;
    {
        std::vector<ETCS::Entity::ChildRef> kids;
        world->getTypedChildRefs(kids);
        for (auto& [tag, rid] : kids)
        {
            ETCS::Entity* k = world->getTypedChild(*tag, rid);
            auto* c = k ? static_cast<Causal_*>(k->getInterfacePointer(ETCS::Buffer("Causal"))) : nullptr;
            if (!c) continue;
            if (c->Solid().shape == CausalSolid::Sphere) ball = c;
            if (c->Space().IsPositive()) cup = c;
        }
    }
    check(wc && cam && anim && ball && cup, "the hole has a ball and a cup");
    if (!(wc && cam && anim && ball && cup)) { std::printf("FAILED\n"); return 1; }
    auto step = [&](int ticks) { for (int i = 0; i < ticks; ++i) { verb(world, "Scene3D.Run", "1, 16"); anim->Advance(); } };

    // -- 1. the course stands ----------------------------------------------------
    std::cout << "\n-- 1. the course stands --\n";
    step(120);
    float b[3];
    where(ball, b);
    check(std::fabs(b[1] - 0.15f) < 0.003f && speed(ball) == 0.0f, "the ball comes to rest on the tee: the field down, the green up");
    check(!wc->Moving(), "...and the world says nothing is moving");

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
    verb(game, "GolfGame.Drag", "512, 440");

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
    // The game sets it back once the card has been read: 2.5 s of frames, as
    // the frame edge would visit (the base measures the time between visits).
    for (int i = 0; i < 80 && static_cast<ETCS::Entity*>(ball)->getParent() != world; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        anim->Advance();
    }
    where(ball, b);
    check(static_cast<ETCS::Entity*>(ball)->getParent() == world && std::fabs(b[2]) < 0.01f && b[1] > 0.0f,
          "the game sets a holed ball back on the tee");

    verb(game, "GolfGame.Delete");
    ETCS::PendingUnloadRegistry::getInstance().join_all();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
