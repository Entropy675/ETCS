/*
 * DrawableOntologyTesterLoader — a compile-and-run proof for the Drawable
 * lineage, with no module, no window and no device anywhere in it. Two
 * concrete leaves are defined right here and allocated straight off the
 * arena, the same isolation MemoryArenaExhausterLoader uses and for the
 * same reason: what is under test is the ONTOLOGY, and a backend in the
 * picture would only mean a second thing that could be wrong.
 *
 * What it proves, in order:
 *
 *   1. A Drawable2D leaf compiles at all -- which is itself the cumulative
 *      -obligation claim, since it could not have been instantiated
 *      without satisfying Surface's verbs and Resizable's GetSize.
 *   2. One inheritance registers FOUR interface pointers, and foreign code
 *      reaching the leaf through "Surface" gets a working Surface_ that
 *      dispatches back to the leaf.
 *   3. Parent-relative addressing composes up the chain, and absolute
 *      position is derived rather than stored.
 *   4. Shape membership is the entire difference between a triangle and
 *      the root: same family, same verbs, different ContainsLocal.
 *   5. drawChildren paints in insertion order while Pick resolves in the
 *      reverse -- so "what you see" and "what you clicked" agree.
 *   6. A camera is a Drawable2D and the projection fills it, so identity is
 *      STABLE by construction; a view that cannot exist is refused rather
 *      than returned empty.
 *   7. Depth belongs to the 3D family and is asked OF a scene, PASSING a
 *      camera -- the span, the per-pixel value and the projection agree,
 *      and moving the eye changes the answer without touching the node.
 */
#undef ETCS_PRODUCTION_BUILD
#ifndef ETCS_MODULE_NAME
#define ETCS_MODULE_NAME "DrawableOntologyTester"
#endif
#include "../ETCS.h"
#include <cmath>
#include <iostream>
#include <string>
#include <vector>
#include <string>

static int g_pass = 0;
static int g_fail = 0;
static void check(bool ok, const std::string& what)
{
    if (ok) { ++g_pass; std::cout << "  ok    " << what << "\n"; }
    else    { ++g_fail; std::cout << "  FAIL  " << what << "\n"; }
}

// Where DrawInto calls land, so ordering is readable without a device.
static std::vector<std::string> g_trace;

// ---------------------------------------------------------------------------
// A rectangular 2D node.
//
// The root and every ordinary box are the SAME class -- the root is just the
// one nobody nested inside anything else. If this file needed a separate
// RootNode type, the self-similarity claim would be false.
// ---------------------------------------------------------------------------
class BoxNode : public Drawable2DBase<BoxNode>
{
public:
    WIRE_TYPE_IDENTITY(BoxNode)

    Rect2D      rect{0, 0, 0, 0};
    std::string label;
    int32_t     z = 0;

    int32_t Order() override { return z; }

    // The ONE comparison Orderable requires. >, <=, >=, == and != are
    // derived from it by OrderableBase -- there is nothing else to declare
    // and no way to make the six disagree.
    bool operator<(const BoxNode& o) const { return z < o.z; }

    Rect2D BoundsConcrete() { return rect; }
    bool   ContainsLocalConcrete(int32_t x, int32_t y)
    {
        return x >= 0 && y >= 0
            && x < static_cast<int32_t>(rect.w)
            && y < static_cast<int32_t>(rect.h);
    }

    WindowSize GetSizeConcrete() { return WindowSize{rect.w, rect.h}; }

    void ClearConcrete(float, float, float, float)                    { g_trace.push_back(label + ":clear"); }
    void DrawRectConcrete(int32_t, int32_t, uint32_t, uint32_t,
                          float, float, float, float)                 { g_trace.push_back(label + ":rect"); }
    void BlitConcrete(Surface_*, int32_t, int32_t,
                      uint32_t, uint32_t, float)                      { g_trace.push_back(label + ":blit"); }

    void DrawIntoConcrete(Surface_* dst)
    {
        g_trace.push_back(label);
        drawChildren(dst);
    }
};

// ---------------------------------------------------------------------------
// A triangular 2D node. Identical obligations, identical verbs; the only
// difference anywhere in this class is which points of its bounding box it
// actually occupies.
// ---------------------------------------------------------------------------
class TriangleNode : public Drawable2DBase<TriangleNode>
{
public:
    WIRE_TYPE_IDENTITY(TriangleNode)

    Rect2D      rect{0, 0, 0, 0};
    std::string label;
    int32_t     z = 0;

    int32_t Order() override { return z; }

    bool operator<(const TriangleNode& o) const { return z < o.z; }

    Rect2D BoundsConcrete() { return rect; }
    // Lower-left half of the box, scaled to its aspect: y*w <= x*h.
    bool ContainsLocalConcrete(int32_t x, int32_t y)
    {
        if (x < 0 || y < 0) return false;
        if (x >= static_cast<int32_t>(rect.w) || y >= static_cast<int32_t>(rect.h)) return false;
        return static_cast<int64_t>(y) * static_cast<int64_t>(rect.w)
            <= static_cast<int64_t>(x) * static_cast<int64_t>(rect.h);
    }

    WindowSize GetSizeConcrete() { return WindowSize{rect.w, rect.h}; }
    void ClearConcrete(float, float, float, float) {}
    void DrawRectConcrete(int32_t, int32_t, uint32_t, uint32_t, float, float, float, float) {}
    void BlitConcrete(Surface_*, int32_t, int32_t, uint32_t, uint32_t, float) {}
    void DrawIntoConcrete(Surface_* dst) { g_trace.push_back(label); drawChildren(dst); }
};

// ---------------------------------------------------------------------------
// A camera: an ordinary 2D plane that a 3D node fills.
//
// Nothing here knows how a scene is rendered, and that is the claim -- the
// camera holds a pose, a lens and a scene RID, and Render is three lines
// that resolve and delegate. Everything below Bounds is the Drawable2D
// membership it gets for free by composing the leaf, which is what lets the
// tests blit it, nest it and pick it exactly like a BoxNode.
// ---------------------------------------------------------------------------
class TestCamera : public CameraBase<TestCamera>
{
public:
    WIRE_TYPE_IDENTITY(TestCamera)

    ViewFrustum view{};
    ETCS::RID   scene_rid = 0;
    Rect2D      rect{0, 0, 320, 240};
    int32_t     z       = 0;
    int         renders = 0;

    int32_t Order() override { return z; }
    bool operator<(const TestCamera& o) const { return z < o.z; }

    void        SetViewConcrete(ViewFrustum v) { view = v; }
    ViewFrustum GetViewConcrete()              { return view; }
    void        SetSceneConcrete(ETCS::RID s)  { scene_rid = s; }
    ETCS::RID   GetSceneConcrete()             { return scene_rid; }

    // Resolve, hand over self, done. By RID and by family name, so a scene
    // built in another module is reached on identical terms -- and a scene
    // that has been destroyed fails to resolve instead of being dereferenced.
    bool RenderConcrete()
    {
        Drawable3D_* scene = ETCS::resolve_in_family<Drawable3D_>("Drawable3D", scene_rid);
        if (!scene) return false;
        ++renders;
        return scene->Project(this) != nullptr;
    }

    Rect2D BoundsConcrete() { return rect; }
    bool   ContainsLocalConcrete(int32_t x, int32_t y)
    {
        return x >= 0 && y >= 0
            && x < static_cast<int32_t>(rect.w) && y < static_cast<int32_t>(rect.h);
    }

    WindowSize GetSizeConcrete() { return WindowSize{rect.w, rect.h}; }
    void ClearConcrete(float, float, float, float) {}
    void DrawRectConcrete(int32_t, int32_t, uint32_t, uint32_t, float, float, float, float) {}
    void BlitConcrete(Surface_*, int32_t, int32_t, uint32_t, uint32_t, float) {}
    void DrawIntoConcrete(Surface_* dst) { g_trace.push_back("camera"); drawChildren(dst); }
};

// ---------------------------------------------------------------------------
// A 3D node that answers all three halves of the camera seam consistently.
//
// Depth is measured along the view direction, which is the only measurement
// this file needs: the point of the tests is that the whole-node span, the
// per-pixel value and the projection agree about the same camera, not that
// anybody can rasterise a box.
// ---------------------------------------------------------------------------
class SceneNode : public Drawable3DBase<SceneNode>
{
public:
    WIRE_TYPE_IDENTITY(SceneNode)

    Box3D box{{-1.0f, -1.0f, -1.0f}, {1.0f, 1.0f, 1.0f}};
    int   projections = 0;
    int32_t z         = 0;

    int32_t Order() override { return z; }
    bool operator<(const SceneNode& o) const { return z < o.z; }

    Box3D Bounds3DConcrete() { return box; }
    bool  ContainsLocal3DConcrete(Point3D p)
    {
        return p.x >= box.min.x && p.x <= box.max.x
            && p.y >= box.min.y && p.y <= box.max.y
            && p.z >= box.min.z && p.z <= box.max.z;
    }

    // Distance from the eye along the normalised view direction. Scene
    // units, the same units DepthSpan and DepthAt are stated in, because
    // three answers in three units are three answers.
    static float depthOf(const ViewFrustum& v, Point3D p)
    {
        float dx = v.look_at.x - v.position.x;
        float dy = v.look_at.y - v.position.y;
        float dz = v.look_at.z - v.position.z;
        float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (len <= 0.0f) return -1.0f;
        dx /= len; dy /= len; dz /= len;
        return (p.x - v.position.x) * dx
             + (p.y - v.position.y) * dy
             + (p.z - v.position.z) * dz;
    }

    // Over the eight corners, so the span is the real extent rather than the
    // centre twice -- the distinction the family's comment insists on.
    DepthSpan DepthForConcrete(Camera_* camera)
    {
        if (!camera) return DepthSpan{-1.0f, -1.0f};
        const ViewFrustum v = camera->GetView();
        float lo = 0.0f, hi = 0.0f;
        bool  first = true;
        for (int i = 0; i < 8; ++i)
        {
            Point3D c{ (i & 1) ? box.max.x : box.min.x,
                       (i & 2) ? box.max.y : box.min.y,
                       (i & 4) ? box.max.z : box.min.z };
            const float d = depthOf(v, c);
            if (first) { lo = hi = d; first = false; }
            else if (d < lo) lo = d;
            else if (d > hi) hi = d;
        }
        return DepthSpan{lo, hi};
    }

    // Flat front face across the frame; negative outside it. A real
    // implementation varies per pixel, but the contract under test is only
    // that "nothing here" is negative and never a plausible distance.
    float DepthAtConcrete(Camera_* camera, int32_t x, int32_t y)
    {
        Drawable2D_* plane = planeOf(camera);
        if (!plane || !plane->ContainsLocal(x, y)) return -1.0f;
        return DepthForConcrete(camera).nearest;
    }

    // The camera's OTHER halves are reached by family name, never by casting
    // Camera_ sideways -- Camera_ declares the view and the scene and nothing
    // else, exactly as Drawable2D_ declares neither. The lineage lives in the
    // Bases, so crossing it is a lookup.
    static Drawable2D_* planeOf(Camera_* camera)
    {
        if (!camera) return nullptr;
        void* p = camera->getInterfacePointer(ETCS::Buffer("Drawable2D"));
        return p ? static_cast<Drawable2D_*>(p) : nullptr;
    }
    static Surface_* surfaceOf(Camera_* camera)
    {
        if (!camera) return nullptr;
        void* p = camera->getInterfacePointer(ETCS::Buffer("Surface"));
        return p ? static_cast<Surface_*>(p) : nullptr;
    }

    // Fills the camera and hands it back as the plane it already is. The
    // identity cannot move: it is the camera.
    Drawable2D_* ProjectConcrete(Camera_* camera)
    {
        Drawable2D_* plane = planeOf(camera);
        Surface_*    surf  = surfaceOf(camera);
        if (!plane || !surf) return nullptr;

        const Rect2D frame = plane->Bounds();
        if (frame.w == 0 || frame.h == 0) return nullptr;
        const ViewFrustum v = camera->GetView();
        if (v.far_plane <= v.near_plane) return nullptr;

        ++projections;
        surf->Clear(0.f, 0.f, 0.f, 1.f);
        return plane;
    }

    WindowSize GetSizeConcrete() { return WindowSize{1, 1}; }
    void ClearConcrete(float, float, float, float) {}
    void DrawRectConcrete(int32_t, int32_t, uint32_t, uint32_t, float, float, float, float) {}
    void BlitConcrete(Surface_*, int32_t, int32_t, uint32_t, uint32_t, float) {}
    void DrawIntoConcrete(Surface_*) {}
};

// ---------------------------------------------------------------------------
// A leaf claiming the three families added alongside the compositor work:
// Clippable (the arithmetic is the family's, applying it is the backend's),
// Glyphs (measure and rasterise, split by WHEN each is needed) and Pointer
// (state, as against InputSource's stream of events).
//
// Deliberately trivial implementations -- what is under test is that the
// families compose, register, and hand a leaf the shared behaviour they
// promise, not that anyone can rasterise a font.
// ---------------------------------------------------------------------------
class Instrument : public ClippableBase<Instrument>,
                   public GlyphsBase<Instrument>,
                   public PointerBase<Instrument>
{
public:
    WIRE_TYPE_IDENTITY(Instrument)

    // Every SetScissor the family issued, so the test can read back that the
    // stack discipline and the intersection happened where they should.
    struct Scissor { int32_t x, y; uint32_t w, h; };
    std::vector<Scissor> applied;

    void SetScissorConcrete(int32_t x, int32_t y, uint32_t w, uint32_t h)
    {
        applied.push_back(Scissor{x, y, w, h});
    }

    TextExtent MeasureTextConcrete(const char* text, uint32_t, uint32_t size_px)
    {
        uint32_t n = 0;
        while (text && text[n]) ++n;
        return TextExtent{ n * (size_px / 2), size_px, (size_px * 3) / 4 };
    }
    TextExtent RasterizeTextConcrete(ETCS::RID, const char* text, uint32_t font,
                                     uint32_t size_px, int32_t, int32_t,
                                     float, float, float, float)
    {
        rasterized = true;
        return MeasureTextConcrete(text, font, size_px);
    }
    bool rasterized = false;

    PointerState state{};
    PointerState ReadPointerConcrete()
    {
        PointerState out = state;
        state.scroll_x = 0.0f;   // deltas are consumed by reading
        state.scroll_y = 0.0f;
        return out;
    }
    bool PointerInsideConcrete() { return inside; }
    bool inside = true;
};

int main()
{
    shell_startup();
    WIRE_CONTEXT();

    auto& arena = ETCS::MemoryArena::getInstance();

    std::cout << "=== Drawable ontology ===\n";

    // -- 1/2. one inheritance, four families ------------------------------
    BoxNode* canvas = arena.allocate<BoxNode>();
    canvas->rect  = Rect2D{0, 0, 800, 600};
    canvas->label = "root";

    check(canvas->getInterfacePointer(ETCS::Buffer("Drawable2D")) != nullptr,
          "leaf registers Drawable2D");
    check(canvas->getInterfacePointer(ETCS::Buffer("Drawable"))   != nullptr,
          "leaf registers Drawable (inherited lineage)");
    check(canvas->getInterfacePointer(ETCS::Buffer("Surface"))    != nullptr,
          "leaf registers Surface (inherited lineage)");
    check(canvas->getInterfacePointer(ETCS::Buffer("Resizable"))  != nullptr,
          "leaf registers Resizable (inherited lineage)");

    // Reached the way foreign code actually reaches it: no static_cast from
    // the concrete type, no knowledge of which leaf answered.
    {
        void* p = canvas->getInterfacePointer(ETCS::Buffer("Surface"));
        Surface_* as_surface = static_cast<Surface_*>(p);
        g_trace.clear();
        as_surface->Clear(0.f, 0.f, 0.f, 1.f);
        check(g_trace.size() == 1 && g_trace[0] == "root:clear",
              "Surface_ interface pointer dispatches back into the leaf");
    }
    {
        void* p = canvas->getInterfacePointer(ETCS::Buffer("Resizable"));
        Resizable_* as_resizable = static_cast<Resizable_*>(p);
        WindowSize s = as_resizable->GetSize();
        check(s.width == 800 && s.height == 600,
              "Resizable_ interface pointer reports the leaf's own size");
    }

    // -- 3. parent-relative addressing composes ---------------------------
    //
    //   root (800x600 @ 0,0)
    //     panel (400x300 @ 100,50)
    //       dot (10x10 @ 20,10)
    //
    // dot's absolute origin is 100+20, 50+10 -- derived, never stored.
    BoxNode* panel = canvas->addTag<BoxNode>();
    panel->rect  = Rect2D{100, 50, 400, 300};
    panel->label = "panel";

    BoxNode* dot = panel->addTag<BoxNode>();
    dot->rect  = Rect2D{20, 10, 10, 10};
    dot->label = "dot";
    dot->z     = 1;

    {
        Point2D in_panel = dot->ToParent(Point2D{0, 0});
        Point2D in_root  = panel->ToParent(in_panel);
        check(in_panel.x == 20 && in_panel.y == 10,
              "child origin in its parent's space");
        check(in_root.x == 120 && in_root.y == 60,
              "absolute position is ToParent composed up the chain");
        Point2D back = dot->ToLocal(panel->ToLocal(in_root));
        check(back.x == 0 && back.y == 0, "ToLocal inverts ToParent");
    }

    // -- 4. shape membership is the only difference ------------------------
    TriangleNode* tri = panel->addTag<TriangleNode>();
    tri->rect  = Rect2D{0, 0, 100, 100};
    tri->label = "tri";
    tri->z     = 2;   // stacked over dot, explicitly

    check(tri->ContainsLocal(90, 10),  "triangle occupies its lower-left half");
    check(!tri->ContainsLocal(10, 90), "triangle does NOT occupy its upper-left corner");
    {
        // Allocated standalone rather than nested under panel: this node is
        // only here to be compared against tri, and adding it to the tree
        // would put it in the ordering test below. Deleting it out of the
        // tree instead would be worse -- MemoryArena::deleteEntity is the
        // direct path that skips DestroyEvent, so the parent's child
        // registry would keep the entry and the next draw would call
        // through it. (Found exactly that way.)
        BoxNode* box_same_extent = arena.allocate<BoxNode>();
        box_same_extent->rect  = Rect2D{0, 0, 100, 100};
        box_same_extent->label = "box_same_extent";
        check(box_same_extent->ContainsLocal(10, 90),
              "a box with the SAME bounds does occupy that corner");
        check(box_same_extent->Bounds().w == tri->Bounds().w
           && box_same_extent->Bounds().h == tri->Bounds().h,
              "identical bounds, different occupancy -- the shape IS ContainsLocal");
        ETCS::MemoryArena::getInstance().deleteEntity(box_same_extent, true);
    }

    // -- 5. paint order forward, pick order reverse ------------------------
    //
    // panel's children, in spawn order: dot, tri. Both are inside panel, and
    // tri's box overlaps dot's position, so a forward-walking picker would
    // answer "dot" for a point where tri is what is actually visible.
    // A real destination: drawChildren refuses a null one (a drawable with
    // nowhere to land is a caller error, not an empty picture), so the sink
    // is an ordinary BoxNode -- which is to say, an ordinary Surface_.
    BoxNode* sink = arena.allocate<BoxNode>();
    sink->rect  = Rect2D{0, 0, 800, 600};
    sink->label = "sink";

    g_trace.clear();
    canvas->DrawInto(static_cast<Surface_*>(
        sink->getInterfacePointer(ETCS::Buffer("Surface"))));
    check(g_trace.size() == 4
       && g_trace[0] == "root" && g_trace[1] == "panel"
       && g_trace[2] == "dot"  && g_trace[3] == "tri",
          "drawChildren paints depth-first, ascending Order()");

    {
        // A point inside panel, inside tri's occupied half, and also inside
        // dot's box: (25, 15) in panel space.
        Point2D p_in_panel{25, 15};
        check(dot->ContainsParent(p_in_panel), "the probe point is inside dot");
        check(tri->ContainsParent(p_in_panel), "the probe point is inside tri as well");

        Drawable2D_* hit = panel->Pick(p_in_panel);
        check(hit == static_cast<Drawable2D_*>(tri),
              "Pick returns the LAST-drawn overlapping node, not the first");

        // Flip the stack and BOTH answers must follow, together. This is
        // the property the shared traversal exists for: paint order and
        // pick order are one rule, not two that happen to agree.
        dot->z = 5;
        g_trace.clear();
        canvas->DrawInto(static_cast<Surface_*>(
            sink->getInterfacePointer(ETCS::Buffer("Surface"))));
        check(g_trace.size() == 4 && g_trace[2] == "tri" && g_trace[3] == "dot",
              "raising a node's Order() repaints it last");
        check(panel->Pick(p_in_panel) == static_cast<Drawable2D_*>(dot),
              "and Pick follows the same flip -- one ordering, two directions");
        dot->z = 1;

        Drawable2D_* miss = panel->Pick(Point2D{-5, -5});
        check(miss == nullptr, "Pick misses cleanly outside the node");

        Drawable2D_* self = panel->Pick(Point2D{380, 280});
        check(self == static_cast<Drawable2D_*>(panel),
              "Pick returns the node itself where no child covers the point");
    }

    // -- 6. projection: the camera IS the plane, and refusal is honest ------
    {
        TestCamera* cam = arena.allocate<TestCamera>();
        ViewFrustum v{};
        v.position      = Point3D{0.f, 0.f, -5.f};
        v.look_at       = Point3D{0.f, 0.f,  0.f};
        v.up            = Point3D{0.f, 1.f,  0.f};
        v.fov_y_radians = 1.0f;
        v.near_plane    = 0.1f;
        v.far_plane     = 100.f;
        cam->SetView(v);

        SceneNode* scene = arena.allocate<SceneNode>();

        // The fan-in a real spawn gets for free. Everything else in this file
        // allocates straight off the arena and never needs it, because it
        // reaches nodes by pointer -- but a camera reaches its scene by RID
        // and family, which is a lookup in the aggregate, and the aggregates
        // are populated post-construction by _make_/addTag, not by the ctor
        // (Entity.h). Calling it here is what makes this the same path a
        // module-built scene takes, rather than a shortcut around it.
        ETCS::etcs_supertype_fanout(scene);

        check(cam->getInterfacePointer(ETCS::Buffer("Camera"))     != nullptr
           && cam->getInterfacePointer(ETCS::Buffer("Drawable2D")) != nullptr
           && cam->getInterfacePointer(ETCS::Buffer("Drawable"))   != nullptr
           && cam->getInterfacePointer(ETCS::Buffer("Surface"))    != nullptr
           && cam->getInterfacePointer(ETCS::Buffer("Orderable"))  != nullptr,
              "a camera registers the whole Drawable2D lineage, not a family of its own");
        check(cam->getInterfacePointer(ETCS::Buffer("Drawable3D")) == nullptr,
              "a camera is NOT a scene node -- the exclusion holds from this side too");

        Drawable2D_* a = scene->Project(cam);
        Drawable2D_* b = scene->Project(cam);
        check(a != nullptr && a == b,
              "Project returns the SAME Drawable2D across calls -- the plane, not a snapshot");
        check(a == static_cast<Drawable2D_*>(
                       static_cast<Drawable2D_*>(cam->getInterfacePointer(ETCS::Buffer("Drawable2D")))),
              "the projected frame IS the camera, reached as an ordinary Drawable2D");
        check(scene->projections == 2, "each call still did the projection work");

        // A plane with no extent and a frustum with no depth are two ways of
        // asking for a view that does not exist; both refuse rather than
        // handing back something empty a caller cannot tell from correct.
        cam->rect.w = 0;
        check(scene->Project(cam) == nullptr,
              "a zero-sized frame returns nullptr, not an empty view");
        cam->rect.w = 320;

        ViewFrustum degenerate = v;
        degenerate.far_plane = degenerate.near_plane;
        cam->SetView(degenerate);
        check(scene->Project(cam) == nullptr,
              "a degenerate frustum returns nullptr too");
        cam->SetView(v);

        check(scene->getInterfacePointer(ETCS::Buffer("Drawable")) != nullptr
           && scene->getInterfacePointer(ETCS::Buffer("Surface"))  != nullptr,
              "a 3D node carries the same lineage as a 2D one");
        check(scene->getInterfacePointer(ETCS::Buffer("Drawable2D")) == nullptr,
              "a 3D node is NOT registered under its sibling family");

        // -- depth: a property of the 3D family, asked OF the scene ---------
        //
        // The box spans z in [-1, 1] with the eye at z = -5 looking down +z,
        // so the near face is 4 away and the far face 6 -- exact, and the
        // whole point of stating depth in scene units rather than clip space.
        DepthSpan span = scene->DepthFor(cam);
        check(std::fabs(span.nearest - 4.0f) < 1e-4f
           && std::fabs(span.furthest - 6.0f) < 1e-4f,
              "DepthFor is a SPAN in scene units, over the node's real extent");

        check(scene->DepthAt(cam, 10, 10) > 0.0f,
              "DepthAt answers inside the camera's frame");
        check(scene->DepthAt(cam, -1, 0) < 0.0f
           && scene->DepthAt(cam, 10000, 0) < 0.0f,
              "outside the frame DepthAt is negative -- never a plausible distance");
        check(std::fabs(scene->DepthAt(cam, 10, 10) - span.nearest) < 1e-4f,
              "the per-pixel and whole-node answers agree about the same camera");

        // Moving the eye changes the depth without touching the node: depth
        // is a fact about a view, which is why it takes the camera.
        ViewFrustum moved = v;
        moved.position = Point3D{0.f, 0.f, -9.f};
        cam->SetView(moved);
        check(scene->DepthFor(cam).nearest > span.nearest,
              "the same node is further from a further camera -- depth is not stored on it");
        cam->SetView(v);

        // -- the camera drives the scene, by RID and by family --------------
        cam->SetScene(scene->getRID());
        check(cam->Render(), "Render resolves the bound scene and delegates to it");
        check(scene->projections == 3, "Render went through Project, not around it");

        cam->SetScene(0);
        check(!cam->Render(),
              "an unbound camera reports false rather than presenting a stale view");

        ETCS::MemoryArena::getInstance().deleteEntity(scene, true);
        ETCS::MemoryArena::getInstance().deleteEntity(cam, true);
    }

    // -- 7. Orderable: one operator required, five derived ----------------
    {
        BoxNode* lo = arena.allocate<BoxNode>();
        BoxNode* hi = arena.allocate<BoxNode>();
        lo->z = 1;
        hi->z = 7;

        check(*lo < *hi,   "operator< is the leaf's own");
        check(*hi > *lo,   "operator> derived from it");
        check(*lo <= *hi,  "operator<= derived");
        check(*hi >= *lo,  "operator>= derived");
        check(*lo != *hi,  "operator!= derived");

        BoxNode* same = arena.allocate<BoxNode>();
        same->z = 1;
        check(*lo == *same,
              "== is EQUIVALENCE under the ordering -- two distinct entities, equal standing");
        check(lo->getRID() != same->getRID(),
              "...and identity is still the RID, which they do not share");

        ETCS::MemoryArena::getInstance().deleteEntity(lo, true);
        ETCS::MemoryArena::getInstance().deleteEntity(hi, true);
        ETCS::MemoryArena::getInstance().deleteEntity(same, true);
    }

    // -- 8. the RIDList orders a homogeneous list by the leaf's operator< --
    //
    // Five boxes under one parent, spawned in an order that has nothing to
    // do with their z. The list they live in is a hash map, so its own
    // enumeration is arbitrary; the ordered view is not.
    {
        BoxNode* stack = arena.allocate<BoxNode>();
        stack->rect  = Rect2D{0, 0, 400, 400};
        stack->label = "stack";

        const int32_t zs[] = {40, 10, 50, 20, 30};
        std::vector<BoxNode*> made;
        for (int i = 0; i < 5; ++i)
        {
            BoxNode* n = stack->addTag<BoxNode>();
            n->rect  = Rect2D{0, 0, 10, 10};
            n->z     = zs[i];
            n->label = "z" + std::to_string(zs[i]);
            made.push_back(n);
        }

        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> ordered;
        stack->getOrderedTypedChildren(ordered);
        bool ascending = ordered.size() == 5;
        int32_t prev = -1;
        for (const auto& e : ordered)
        {
            ETCS::Entity* c = stack->getTypedChild(e.first, e.second);
            if (!c) { ascending = false; break; }
            int32_t z = static_cast<BoxNode*>(c->getTrueType())->z;
            if (z < prev) ascending = false;
            prev = z;
        }
        check(ascending, "RIDList orders a homogeneous list by the pointee's operator<");

        // The stale case no container can see: the key moves, membership
        // does not. Without the explicit seam the view keeps its old answer.
        made[2]->z = 5;    // was 50, the last; now 5, the first
        made[2]->Reorder();

        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> after;
        stack->getOrderedTypedChildren(after);
        ETCS::Entity* first = after.empty()
            ? nullptr : stack->getTypedChild(after[0].first, after[0].second);
        check(first != nullptr
           && static_cast<BoxNode*>(first->getTrueType())->z == 5,
              "Reorder() re-establishes position after the key moves under the list");

        // And the seam that needs no cooperation: a push marks it stale by
        // itself, so a newly attached child lands in the right place.
        BoxNode* late = stack->addTag<BoxNode>();
        late->rect  = Rect2D{0, 0, 10, 10};
        late->z     = -1;
        late->label = "late";

        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> after_push;
        stack->getOrderedTypedChildren(after_push);
        ETCS::Entity* now_first = after_push.empty()
            ? nullptr : stack->getTypedChild(after_push[0].first, after_push[0].second);
        check(after_push.size() == 6
           && now_first != nullptr
           && static_cast<BoxNode*>(now_first->getTrueType())->z == -1,
              "an insert marks the view stale by itself -- no Reorder() needed at a seam");

        ETCS::MemoryArena::getInstance().deleteEntity(stack, true);
    }

    // -- 9. Clippable: intersect on push, restore on pop -------------------
    {
        Instrument* ins = arena.allocate<Instrument>();

        check(ins->getInterfacePointer(ETCS::Buffer("Clippable")) != nullptr
           && ins->getInterfacePointer(ETCS::Buffer("Glyphs"))    != nullptr
           && ins->getInterfacePointer(ETCS::Buffer("Pointer"))   != nullptr,
              "one leaf, three independent families registered");

        ins->PushClip(100, 100, 200, 200);
        check(ins->applied.size() == 1
           && ins->applied[0].x == 100 && ins->applied[0].w == 200,
              "PushClip applies the requested region when nothing is in effect");

        // Nested: overlaps the first from (200,150), so the intersection is
        // (200,150)-(300,300) = 100x150. A push can only ever shrink.
        ins->PushClip(200, 150, 400, 400);
        check(ins->applied.size() == 2
           && ins->applied[1].x == 200 && ins->applied[1].y == 150
           && ins->applied[1].w == 100 && ins->applied[1].h == 150,
              "a nested PushClip INTERSECTS rather than replaces");

        ins->PopClip();
        check(ins->applied.size() == 3
           && ins->applied[2].x == 100 && ins->applied[2].w == 200,
              "PopClip restores the region the matching push replaced");

        // Disjoint: nothing in common, so the region is empty -- and empty
        // must stay empty rather than underflowing into enormous.
        ins->PushClip(1000, 1000, 50, 50);
        check(ins->applied.back().w == 0 && ins->applied.back().h == 0,
              "a disjoint clip is EMPTY, not an unsigned underflow");

        ins->PopClip();
        ins->PopClip();
        check(!ins->HasClip(), "the stack empties");
        const size_t before = ins->applied.size();
        ins->PopClip();
        check(ins->applied.size() == before,
              "popping an empty stack is a no-op, not a crash inside a draw loop");

        // -- 10. Glyphs: measure without pixels, rasterise with them -------
        const TextExtent m = ins->MeasureText("hello", 0, 16);
        check(m.width == 5 * 8 && m.height == 16 && m.baseline == 12,
              "MeasureText answers with no surface in sight -- layout can run first");
        check(!ins->rasterized, "...and measuring rasterised nothing");

        const TextExtent r = ins->RasterizeText(0, "hello", 0, 16, 0, 0, 1, 1, 1, 1);
        check(ins->rasterized, "RasterizeText produced pixels");
        check(r.width == m.width && r.height == m.height && r.baseline == m.baseline,
              "and agreed with what Measure promised -- the property layout depends on");

        // -- 11. Pointer: state, and deltas consumed by reading ------------
        ins->state = PointerState{ 640, 480, 0x1, 0.0f, -3.0f };
        const PointerState p1 = ins->ReadPointer();
        check(p1.x == 640 && p1.y == 480 && (p1.buttons & 0x1) != 0,
              "Pointer reports position and buttons together, as one value");
        check(p1.scroll_y == -3.0f, "the scroll delta arrives once");
        const PointerState p2 = ins->ReadPointer();
        check(p2.scroll_y == 0.0f && p2.x == 640,
              "...and reading consumed it, while the POSITION persists");
        check(ins->PointerInside(), "PointerInside is a separate question from the coordinates");

        ETCS::MemoryArena::getInstance().deleteEntity(ins, true);
    }

    ETCS::MemoryArena::getInstance().deleteEntity(canvas, true);
    ETCS::MemoryArena::getInstance().deleteEntity(sink, true);

    std::cout << "=== Summary: " << g_pass << " passed, " << g_fail << " failed ===\n";
    return g_fail == 0 ? 0 : 1;
}
