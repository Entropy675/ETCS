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
 *   6. Project returns a Drawable2D with STABLE identity, and refuses a
 *      degenerate camera rather than inventing an empty view.
 */
#undef ETCS_PRODUCTION_BUILD
#ifndef ETCS_MODULE_NAME
#define ETCS_MODULE_NAME "DrawableOntologyTester"
#endif
#include "../ETCS.h"
#include <iostream>
#include <string>
#include <vector>

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
// A 3D node whose projection lands on a plane it does not own.
// ---------------------------------------------------------------------------
class SceneNode : public Drawable3DBase<SceneNode>
{
public:
    WIRE_TYPE_IDENTITY(SceneNode)

    Box3D        box{{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}};
    Drawable2D_* plane       = nullptr;
    int          projections = 0;

    Box3D Bounds3DConcrete() { return box; }
    bool  ContainsLocal3DConcrete(Point3D p)
    {
        return p.x >= box.min.x && p.x <= box.max.x
            && p.y >= box.min.y && p.y <= box.max.y
            && p.z >= box.min.z && p.z <= box.max.z;
    }

    // The same plane every call. Contents update; identity does not move.
    Drawable2D_* ProjectConcrete(CameraSetup camera)
    {
        if (camera.frame_width == 0 || camera.frame_height == 0) return nullptr;
        ++projections;
        return plane;
    }

    WindowSize GetSizeConcrete() { return WindowSize{1, 1}; }
    void ClearConcrete(float, float, float, float) {}
    void DrawRectConcrete(int32_t, int32_t, uint32_t, uint32_t, float, float, float, float) {}
    void BlitConcrete(Surface_*, int32_t, int32_t, uint32_t, uint32_t, float) {}
    void DrawIntoConcrete(Surface_*) {}
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

    // -- 6. projection: stable identity, honest refusal ---------------------
    {
        BoxNode* image_plane = arena.allocate<BoxNode>();
        image_plane->rect  = Rect2D{0, 0, 320, 240};
        image_plane->label = "image_plane";

        SceneNode* scene = arena.allocate<SceneNode>();
        scene->plane = image_plane;

        CameraSetup cam{};
        cam.position      = Point3D{0.f, 0.f, -5.f};
        cam.look_at       = Point3D{0.f, 0.f,  0.f};
        cam.up            = Point3D{0.f, 1.f,  0.f};
        cam.fov_y_radians = 1.0f;
        cam.near_plane    = 0.1f;
        cam.far_plane     = 100.f;
        cam.frame_width   = 320;
        cam.frame_height  = 240;

        Drawable2D_* a = scene->Project(cam);
        Drawable2D_* b = scene->Project(cam);
        check(a != nullptr && a == b,
              "Project returns the SAME Drawable2D across calls -- the plane, not a snapshot");
        check(a == static_cast<Drawable2D_*>(image_plane),
              "the projected frame is an ordinary Drawable2D like any other");
        check(scene->projections == 2, "each call still did the projection work");

        CameraSetup degenerate = cam;
        degenerate.frame_width = 0;
        check(scene->Project(degenerate) == nullptr,
              "a camera that cannot be realised returns nullptr, not an empty view");

        check(scene->getInterfacePointer(ETCS::Buffer("Drawable")) != nullptr
           && scene->getInterfacePointer(ETCS::Buffer("Surface"))  != nullptr,
              "a 3D node carries the same lineage as a 2D one");
        check(scene->getInterfacePointer(ETCS::Buffer("Drawable2D")) == nullptr,
              "a 3D node is NOT registered under its sibling family");

        ETCS::MemoryArena::getInstance().deleteEntity(scene, true);
        ETCS::MemoryArena::getInstance().deleteEntity(image_plane, true);
    }

    ETCS::MemoryArena::getInstance().deleteEntity(canvas, true);
    ETCS::MemoryArena::getInstance().deleteEntity(sink, true);

    std::cout << "=== Summary: " << g_pass << " passed, " << g_fail << " failed ===\n";
    return g_fail == 0 ? 0 : 1;
}
