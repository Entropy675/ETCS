// GlowTesterLoader.cc
//
// A GLOWING NODE (Scene3D::SetGlow), drawn by the host rasteriser and read
// back from the camera's own pixels. A glow gives light instead of taking
// it: its colour times its strength, ADDED to what is behind it, strongest
// where its surface faces the eye and fading to nothing where it turns
// edge-on; tested against the solids' depth, writing none of its own. Held
// to account, pixel by pixel:
//
//   1. ADDED. A green glow over the sky leaves the sky's red and blue as they
//      were and adds green, most at its middle.
//   2. SOFT. Toward its silhouette it adds less and less: no hard outline.
//   3. BEHIND A SOLID, NOT SEEN; IN FRONT, NOT HIDING. A glow behind a red
//      block leaves the block as it was; one in front of it adds to the
//      block's colour and the block still shows through.
//   4. ORDER IS NOT THE WALK'S. A solid made after a glow, in front of it,
//      still hides it: the glow is drawn after every solid.
//   5. SetGlow 0 is a lit solid again: it hides what is behind it.
//   6. SMOKE (Scene3D::SetSmoke) is laid OVER what is behind it: dark smoke
//      over the sky darkens it, most at its middle, least at its rim.
//   7. FARTHEST FIRST. Smoke in front of a glow dims it; a glow in front of
//      smoke shines on undimmed -- whichever was made first.
//
// The device paths (shaders/mesh.vert + mesh.frag, the WebGPU mesh pass) do
// the same sum; this reads the one path whose pixels a loader can hold.
//
//   ./Run_GlowTesterLoader
//
// No display, no GPU.

#include "../ETCS.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

int main()
{
    WIRE_CONTEXT();
    auto verb = [&](ETCS::Entity* e, const char* v, const std::string& args) { e->call(v, args.c_str(), ctx); };
    std::printf("-- a glowing node, on the host --\n");

    ETCS::Entity* world = ETCS::spawn_entity("RenderProvider", "Scene3D", env, loader);
    ETCS::Entity* eye   = ETCS::spawn_entity("RenderProvider", "Camera3D", env, loader);
    if (!world || !eye) { std::printf("FAILED (no RenderProvider)\n"); return 1; }
    verb(world, "Scene3D.Create", "1, 1, 1");
    verb(world, "Scene3D.SetVisible", "0");
    verb(eye, "Camera3D.Create", "128, 128");
    verb(eye, "Camera3D.SetBackground", "0.2, 0.1, 0.3, 1.0");
    verb(eye, "Camera3D.SetLens", "60, 0.1, 100");
    verb(eye, "Camera3D.LookAt", "0, 0, -10, 0, 0, 0");
    verb(eye, "Camera3D.SetScene", std::to_string(world->getRID()));

    std::vector<ETCS::Entity*> made;
    // A sphere for the glows; a red block on the right; glows on the left in
    // the open, and two over the block -- one behind it, one in front.
    ETCS::Entity* ball = ETCS::make_typed_child("RenderProvider", "Mesh", world, loader);
    verb(ball, "Mesh.Create", "");
    verb(ball, "Mesh.Sphere", "16");
    auto node = [&](const char* at, const char* size, const char* colour) {
        ETCS::Entity* n = ETCS::make_typed_child("RenderProvider", "Scene3D", world, loader);
        verb(n, "Scene3D.Create", size);
        verb(n, "Scene3D.SetPosition", at);
        verb(n, "Scene3D.SetColor", colour);
        made.push_back(n);
        return n;
    };
    ETCS::Entity* open  = node("-2.5, 0, 0", "3, 3, 3", "0.0, 0.8, 0.0, 1.0");
    ETCS::Entity* behind = node("2.5, 1.2, 3", "2, 2, 2", "0.0, 0.8, 0.0, 1.0");
    node("2.5, 0, 0", "3, 4, 1", "0.9, 0.1, 0.1, 1.0");   // the block
    ETCS::Entity* front = node("2.5, -1.2, -2", "1.5, 1.5, 1.5", "0.0, 0.8, 0.0, 1.0");
    for (ETCS::Entity* g : { open, behind, front })
    {
        verb(g, "Scene3D.SetMesh", std::to_string(ball->getRID()));
        verb(g, "Scene3D.SetGlow", "1.0");
    }
    // Made after the glow in the open, in front of its lower half: a solid.
    ETCS::Entity* cover = node("-2.5, -1.0, -2", "3.4, 1.0, 0.5", "0.1, 0.1, 0.9, 1.0");
    (void)cover;

    Pixels_* px = static_cast<Pixels_*>(eye->getInterfacePointer(ETCS::Buffer("Pixels")));
    auto shot = [&]() { verb(eye, "Camera3D.Render", ""); };
    // The pixel under a world point on the z = 0 plane (the camera is 10 back).
    auto at = [&](float wx, float wy, float wz) {
        const float t = 0.5f / std::tan(30.0f * 3.14159265f / 180.0f);
        const float d = wz + 10.0f;
        const int x = static_cast<int>(64.0f + (wx / d) * t * 128.0f), y = static_cast<int>(64.0f - (wy / d) * t * 128.0f);
        const uint8_t* p = px->PixelData() + static_cast<size_t>(y) * px->PixelStride() + static_cast<size_t>(x) * 4;
        return std::array<int, 3>{ p[0], p[1], p[2] };
    };
    shot();
    if (!px || !px->PixelData()) { std::printf("FAILED (the camera has no pixels)\n"); return 1; }
    const auto sky = at(0, 4.5f, 0);
    const auto mid = at(-2.5f, 0.6f, 0), rim = at(-2.5f + 1.2f, 0.6f, 0);   // 1.34 out of 1.5
    const auto block_bare = at(2.5f, 0.0f, 0), block_behind = at(2.5f, 1.2f, 0), block_front = at(2.5f, -1.2f, 0);
    const auto covered = at(-2.5f, -1.0f, -2);
    std::printf("        (sky %d %d %d; glow middle %d %d %d, near its rim %d %d %d)\n",
                sky[0], sky[1], sky[2], mid[0], mid[1], mid[2], rim[0], rim[1], rim[2]);
    std::printf("        (block %d %d %d; a glow behind it %d %d %d; one in front %d %d %d; the cover %d %d %d)\n",
                block_bare[0], block_bare[1], block_bare[2], block_behind[0], block_behind[1], block_behind[2],
                block_front[0], block_front[1], block_front[2], covered[0], covered[1], covered[2]);

    check(mid[0] == sky[0] && mid[2] == sky[2] && mid[1] > sky[1] + 120, "a glow ADDS its colour to the sky behind it, most at its middle");
    check(rim[0] == sky[0] && rim[1] > sky[1] && rim[1] < mid[1] - 60, "...and less toward its silhouette: no hard outline");
    check(block_behind == block_bare, "a glow behind a solid is not seen");
    check(block_front[0] == block_bare[0] && block_front[2] == block_bare[2] && block_front[1] > block_bare[1] + 60,
          "one in front adds to it, and the solid still shows through");
    check(covered[1] < 40 && covered[2] > 150, "a solid made after a glow, in front of it, hides it: glows are drawn last");

    verb(open, "Scene3D.SetGlow", "0");
    shot();
    const auto solid = at(-2.5f, 0.6f, 0);
    std::printf("        (glow 0: %d %d %d)\n", solid[0], solid[1], solid[2]);
    check(solid[0] < 10 && solid[2] < 10 && solid[1] > 50 && solid[1] != mid[1], "SetGlow 0 is a lit solid again: green, over nothing");

    // Smoke, on a clear stage: everything before it put away.
    for (ETCS::Entity* n : made) verb(n, "Scene3D.SetVisible", "0");
    auto soft = [&](const char* at, const char* size, const char* colour, const char* verbname, const char* amount) {
        ETCS::Entity* n = node(at, size, colour);
        verb(n, "Scene3D.SetMesh", std::to_string(ball->getRID()));
        verb(n, verbname, amount);
        return n;
    };
    soft("-3.5, 0, 0", "2.4, 2.4, 2.4", "0.05, 0.05, 0.05, 1.0", "Scene3D.SetSmoke", "0.9");
    // Below: smoke in front of a glow, the smoke made first. Above: a glow in
    // front of smoke, the glow made first.
    soft("0, -2.2, -1", "2.4, 2.4, 2.4", "0.05, 0.05, 0.05, 1.0", "Scene3D.SetSmoke", "0.9");
    soft("0, -2.2, 2", "2, 2, 2", "0.0, 0.8, 0.0, 1.0", "Scene3D.SetGlow", "0.5");
    soft("0, 2.2, -1", "2, 2, 2", "0.0, 0.8, 0.0, 1.0", "Scene3D.SetGlow", "0.5");
    soft("0, 2.2, 2", "3.6, 3.6, 3.6", "0.05, 0.05, 0.05, 1.0", "Scene3D.SetSmoke", "0.9");
    shot();
    const auto sky2 = at(-3.5f, 4.5f, 0), smoke_mid = at(-3.5f, 0, 0), smoke_rim = at(-3.5f + 1.0f, 0, 0);
    const auto dimmed = at(0, -2.2f, 2), shining = at(0, 2.2f, -1);
    std::printf("        (sky %d %d %d; dark smoke middle %d %d %d, near its rim %d %d %d)\n",
                sky2[0], sky2[1], sky2[2], smoke_mid[0], smoke_mid[1], smoke_mid[2], smoke_rim[0], smoke_rim[1], smoke_rim[2]);
    std::printf("        (a glow behind smoke %d %d %d; one in front of smoke %d %d %d)\n",
                dimmed[0], dimmed[1], dimmed[2], shining[0], shining[1], shining[2]);
    check(smoke_mid[0] < sky2[0] - 15 && smoke_mid[2] < sky2[2] - 25 && smoke_rim[2] > smoke_mid[2] + 10 && smoke_rim[2] < sky2[2],
          "dark smoke is laid over the sky: darker, most at its middle, less toward its rim");
    check(dimmed[1] < 60 && shining[1] > dimmed[1] + 120,
          "smoke in front of a glow dims it; a glow in front of smoke does not -- farthest first, not the order made");

    verb(eye, "Camera3D.Delete", "");
    verb(world, "Scene3D.Delete", "");
    ETCS::PendingUnloadRegistry::getInstance().join_all();
    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED", g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
