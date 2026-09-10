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
// A 2D node that also claims Lifecycle and Threaded, so the two lifecycle
// funnels that mark -- Release and Halt -- have something to fire on. Nothing
// else in this file claims either alongside Observable, which is what the
// checks below need.
// ---------------------------------------------------------------------------
class ReleasableNode : public Drawable2DBase<ReleasableNode>,
                       public LifecycleBase<ReleasableNode>,
                       public ThreadedBase<ReleasableNode>
{
public:
    WIRE_TYPE_IDENTITY(ReleasableNode)

    int32_t z = 0;
    int32_t Order() override { return z; }
    bool operator<(const ReleasableNode& o) const { return z < o.z; }

    Rect2D     BoundsConcrete() { return Rect2D{0, 0, 1, 1}; }
    bool       ContainsLocalConcrete(int32_t, int32_t) { return true; }
    WindowSize GetSizeConcrete() { return WindowSize{1, 1}; }

    void ClearConcrete(float, float, float, float) {}
    void DrawRectConcrete(int32_t, int32_t, uint32_t, uint32_t,
                          float, float, float, float) {}
    void BlitConcrete(Surface_*, int32_t, int32_t, uint32_t, uint32_t, float) {}
    void DrawIntoConcrete(Surface_* dst) { drawChildren(dst); }

    bool released_ran = false;
    void ReleaseConcrete() { released_ran = true; }
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
    // The device toggle: preference stored, effective mode derived (Camera.h).
    // True by default -- a camera uses a device when it has one.
    bool m_want_device = true;
    bool SetDeviceProjectionConcrete(bool on) { m_want_device = on; return on == m_want_device; }
    bool DeviceProjectionRequestedConcrete() const { return m_want_device; }

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
// A CPU-backed surface, which is all this needs to be: Drawable2D gives it
// Surface, Surface gives it Resizable, and Resizable gives it Observable. So
// the observation tests below reach it exactly the way a device would.
class PixelNode : public Drawable2DBase<PixelNode>,
                  public PixelsBase<PixelNode>
{
public:
    WIRE_TYPE_IDENTITY(PixelNode)

    int32_t z = 0;
    int32_t Order() override { return z; }
    bool operator<(const PixelNode& o) const { return z < o.z; }

    Rect2D BoundsConcrete() { return Rect2D{0, 0, PixelWidth(), PixelHeight()}; }
    bool   ContainsLocalConcrete(int32_t, int32_t) { return true; }
    WindowSize GetSizeConcrete() { return WindowSize{PixelWidth(), PixelHeight()}; }

    // Resizable's verb, so PollResize has something to land on.
    bool resized = false;
    bool ResizeTo(WindowSize s) override
    {
        Allocate(s.width, s.height);
        resized = true;
        return true;
    }

    // A stand-in for a window manager delivering a size, so the settle
    // countdown can be driven without one.
    void deliverSize(WindowSize s) { notifyResize(s); }

    // Resizable_'s own record, which is what a follower would read. Distinct
    // from GetSize here only because this leaf's size is its pixel buffer.
    WindowSize recordedSize() const { return m_size; }

    void ClearConcrete(float r, float g, float b, float a) { ClearTo(r, g, b, a); }
    void DrawRectConcrete(int32_t x, int32_t y, uint32_t w, uint32_t h,
                          float r, float g, float b, float a) { FillRect(x, y, w, h, r, g, b, a); }
    void BlitConcrete(Surface_*, int32_t, int32_t, uint32_t, uint32_t, float) {}
    void DrawIntoConcrete(Surface_*) {}
};

// ---------------------------------------------------------------------------
// The other side of the raster split (ontology/Raster.h): a Drawable2D whose
// pixels are somewhere this process cannot address. VulkanSurface is the real
// one; this is the same claim with the device replaced by two numbers, which
// is all the ontology can see of a device anyway.
//
// Drawable2D as well as Renderable, deliberately -- the coordinate-origin walk
// in RenderProvider stops at the first ancestor that is a RASTER, and the case
// that used to be walked straight past is exactly this one: a drawable node
// with a raster that is not a Pixels.
//
// It does NOT claim PixelsBase, and it cannot: both families inherit Raster_
// virtually, so the two collapse to ONE Raster_ with PixelWidth() overridden
// in two sibling branches and neither dominating -- no unique final overrider,
// which is a compile error rather than a runtime check. So no test below
// asserts it; this class not compiling if the line were added IS the test.
// ---------------------------------------------------------------------------
class DeviceNode : public Drawable2DBase<DeviceNode>,
                   public RenderableBase<DeviceNode>
{
public:
    WIRE_TYPE_IDENTITY(DeviceNode)

    uint64_t device = 0;
    uint32_t dw = 0, dh = 0;

    int32_t z = 0;
    int32_t Order() override { return z; }
    bool operator<(const DeviceNode& o) const { return z < o.z; }

    uint64_t DeviceKeyConcrete()   const { return device; }
    uint32_t PixelWidthConcrete()  const { return dw; }
    uint32_t PixelHeightConcrete() const { return dh; }

    Rect2D BoundsConcrete() { return Rect2D{0, 0, dw, dh}; }
    bool   ContainsLocalConcrete(int32_t, int32_t) { return true; }
    WindowSize GetSizeConcrete() { return WindowSize{dw, dh}; }

    void ClearConcrete(float, float, float, float) {}
    void DrawRectConcrete(int32_t, int32_t, uint32_t, uint32_t,
                          float, float, float, float) {}
    void BlitConcrete(Surface_*, int32_t, int32_t, uint32_t, uint32_t, float) {}
    void DrawIntoConcrete(Surface_*) {}
};

// A device that is nothing but its own key -- which is all the ontology can
// see of one. Zero means "not created yet", the ordinary state of an entity
// between spawn and Create, and the reason DeviceSource skips it.
class TestDevice : public DeviceBase<TestDevice>
{
public:
    WIRE_TYPE_IDENTITY(TestDevice)

    uint64_t key = 0;
    uint64_t DeviceKeyConcrete()   const { return key; }
    bool     DeviceReadyConcrete() const { return key != 0; }
};

// ---------------------------------------------------------------------------
// A transform stage: one step, and a forward link.
//
// Claims MatrixBase, which composes WrapperBase -- so it carries the "Wrapper"
// tag and interface pointer without asking, and MirrorBuffer's chain
// resolution finds it as an ordinary Wrapper child. Nothing here registers
// anything.
//
// WRAP MULTIPLIES, UNWRAP DOES NOT, and that asymmetry is the point rather
// than an omission: the two sides of the boundary are two spaces, so arriving
// is not undoing (ontology/Matrix.h). Which side does the work is this leaf's
// choice, not the family's.
// ---------------------------------------------------------------------------
class Xform : public MatrixBase<Xform>
{
public:
    WIRE_TYPE_IDENTITY(Xform)

    size_t wraps = 0, unwraps = 0;

    // The stage's own value, loaded through the typed bridge so the shape is
    // stated once and checked by the compiler.
    void Become(const Matrix4& m) { Load(m); }
    void BecomeShape(uint32_t r, uint32_t c) { SetShape(r, c); }

    // The *Concrete half of the family's dispatch, as every other leaf owes.
    void WrapConcrete(ETCS::MBuffer&, ETCS::SignalContext)   { ++wraps; }
    void UnwrapConcrete(ETCS::MBuffer&, ETCS::SignalContext) { ++unwraps; }
    void CloseConcrete(ETCS::MBuffer&, ETCS::SignalContext)  {}
    ETCS::WireScope ScopeConcrete() const { return ETCS::WireScope::LMAX; }
};

// A minimal actor: claims Thread, so it gets Threaded (Halt/Halted/Shape), a
// SignalContext and a closure without writing any of them.
class Worker : public ThreadBase<Worker>
{
public:
    WIRE_TYPE_IDENTITY(Worker)

    ETCS::Buffer script{"none"};
    ETCS::Buffer ScriptConcrete() { return script; }

    // Children are real entities, so the DAG is the parent edge.
    uint64_t DetachConcrete(const ETCS::Buffer& s)
    {
        Worker* child = this->addTag<Worker>();   // the Halted guard is ThreadBase's
        if (!child) return 0;
        child->script = s;
        InheritClosureTo(*child);            // the copy it carries into its own lifetime
        return child->getRID();
    }
};

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

    // -- 8b. search_ordered: bisect the order, and return the RANGE ----------
    //
    // The order is what makes a search possible, so this is the only search
    // the ontology can offer -- and because Orderable's == is EQUIVALENCE
    // rather than identity, matching has to return every entity that shares a
    // standing, not one of them. Three boxes at z=20 is the case that tells a
    // find from an equal_range, so three is what this builds.
    {
        ETCS::RIDList<BoxNode*> list(ETCS::MemoryArena::getInstance());
        const int32_t zs[] = {40, 20, 10, 20, 30, 20};
        std::vector<BoxNode*> nodes;
        for (int i = 0; i < 6; ++i)
        {
            BoxNode* n = arena.allocate<BoxNode>();
            n->rect = Rect2D{0, 0, 10, 10};
            n->z = zs[i];
            nodes.push_back(n);
            list.insert(static_cast<ETCS::RID>(1000 + i), n);
        }

        std::vector<ETCS::RID> hits;
        BoxNode probe;  probe.z = 20;
        list.search_ordered(probe, hits);
        check(hits.size() == 3, "search_ordered returns the whole equivalence range ("
              + std::to_string(hits.size()) + " of 3 at z=20)");

        bool all20 = !hits.empty();
        for (ETCS::RID r : hits) if (list.get_typed(r)->z != 20) all20 = false;
        check(all20, "and every entity it returns actually has that standing");

        hits.clear();
        probe.z = 40;
        list.search_ordered(probe, hits);
        check(hits.size() == 1, "a standing held by one entity returns exactly it");

        hits.clear();
        probe.z = 999;
        list.search_ordered(probe, hits);
        check(hits.empty(), "a standing nothing holds returns nothing, not the nearest");

        // The bound the bisection depends on: search AFTER a key moves must see
        // the new position, or it is reading a stale index.
        nodes[0]->z = 20;              // was 40, alone; now a fourth at 20
        list.reorder();
        hits.clear();
        probe.z = 20;
        list.search_ordered(probe, hits);
        check(hits.size() == 4, "after Reorder the search sees the new standing ("
              + std::to_string(hits.size()) + " of 4)");
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

    // -- 12. Observable: the dirty bit is an EDGE, not a state -------------
    //
    // The whole reason the family exists. Everything here fails against a
    // single read-and-clear flag on the observed, which is what this replaced.
    {
        PixelNode* src = arena.allocate<PixelNode>();
        src->Allocate(8, 8);

        void* op = src->getInterfacePointer(ETCS::Buffer("Observable"));
        check(op != nullptr, "a Pixels leaf is Observable (via Surface -> Resizable)");
        ETCS::IWireObservable* obs = static_cast<ETCS::IWireObservable*>(op);

        // Two devices over one image -- paint_two_windows in miniature.
        const uint64_t devA = 1001, devB = 1002;
        obs->Observe(devA);
        obs->Observe(devB);

        check(obs->TakeObserved(devA), "a new observer starts dirty -- it has nothing cached");
        check(obs->TakeObserved(devB), "...and so does the second, independently");
        check(!obs->TakeObserved(devA), "reading clears it FOR THAT OBSERVER");

        src->FillRect(0, 0, 4, 4, 1.f, 0.f, 0.f, 1.f);
        check(obs->TakeObserved(devA), "a write tells the first observer");
        check(obs->TakeObserved(devB),
              "AND the second -- one flag let whichever looked first eat the other's");
        check(!obs->TakeObserved(devB), "...and both settle again");

        check(obs->TakeObserved(9999),
              "an unregistered observer is told true: no edge, so no claim to be current");

        // Registration is what an edge IS, so dropping it drops the bit.
        obs->Unobserve(devA);
        src->FillRect(0, 0, 2, 2, 0.f, 1.f, 0.f, 1.f);
        check(obs->TakeObserved(devB), "an observer still registered still hears");
        check(src->Observed(), "Observed() is 'is anyone watching'");
        obs->Unobserve(devB);
        check(!src->Observed(), "...and false once the last edge is gone");

        ETCS::MemoryArena::getInstance().deleteEntity(src, true);
    }

    // -- 13. Observable: self-observation settles --------------------------
    //
    // A node caching something derived from its own subtree registers an edge
    // to ITSELF -- not intrinsic, since a node is not its own parent. This is
    // the check that catches a forgotten ObserveSelf, whose only other symptom
    // is recomposing every frame forever with correct output the whole time.
    {
        PixelNode* node = arena.allocate<PixelNode>();
        node->Allocate(4, 4);
        node->ObserveSelf();

        check(node->TakeObserved(node->getRID()), "a self-observer starts dirty");
        check(!node->TakeObserved(node->getRID()),
              "...and SETTLES -- an unregistered one would answer true forever");

        // A CHILD changing is what a self-observer is watching for. Its own
        // write is not -- see below.
        PixelNode* kid = node->addTag<PixelNode>();
        kid->Allocate(2, 2);
        (void)node->TakeObserved(node->getRID());
        kid->FillRect(0, 0, 1, 1, 0.f, 0.f, 1.f, 1.f);
        check(node->TakeObserved(node->getRID()), "its own subtree changing wakes it");

        node->FillRect(0, 0, 1, 1, 1.f, 1.f, 1.f, 1.f);
        check(!node->TakeObserved(node->getRID()),
              "a node's own write is not news to it -- skipped at the source, no clear needed");

        /*
         * A CHILD BEING DELETED IS A SUBTREE CHANGE, exactly as much as that
         * child writing a pixel was two checks ago. Nothing in the tree can
         * tell the difference from the outside: in both cases what the parent
         * would compose is no longer what it composed last time.
         *
         * Deliberately settled first, so this asserts the DELETE woke it and
         * not something earlier. Symmetric with the FillRect check above,
         * which is the same assertion for the other kind of change.
         */
        // getOwningArena(), not getInstance(): a child allocated through
        // addTag<T> lives in its PARENT's arena, and deleteEntity only walks
        // the dtor list of the arena it is called on -- so the global one is
        // a silent no-op here, which is exactly how this check first passed
        // for the wrong reason.
        (void)node->TakeObserved(node->getRID());
        kid->getOwningArena().deleteEntity(kid, true);
        check(node->TakeObserved(node->getRID()),
              "a child being DELETED wakes its parent -- a subtree that lost a "
              "node composes differently than one that did not");

        ETCS::MemoryArena::getInstance().deleteEntity(node, true);
    }

    // -- 13a2. The rest of the state surface -------------------------------
    //
    // Observable is declared in core ahead of the families that claim it so a
    // type inherits observation of the runtime's OWN transitions. The surface
    // is the tags: origin-affixed ones and flags. These are the funnels that
    // move it, other than the tag write already covered above.
    {
        PixelNode* parent = arena.allocate<PixelNode>();
        parent->Allocate(4, 4);
        parent->ObserveSelf();
        (void)parent->TakeObserved(parent->getRID());

        // CREATION. The mirror of the delete check: a typed child is an
        // origin-affixed tag appearing, so it moves the surface. Marked in the
        // one funnel every addTag<T> passes through, not by each leaf type's
        // Create body happening to set a flag.
        PixelNode* born = parent->addTag<PixelNode>();
        check(parent->TakeObserved(parent->getRID()),
              "a child ENTERING wakes its parent -- creation is a tag edge");

        // A BARE IS-A MARKER IS NOT PART OF THE SURFACE, because it is not
        // mutable in either direction. Removing it would not remove the
        // family -- the interface pointer is fixed at construction -- it would
        // only desynchronise hasTag from what the type actually is.
        check(born->hasTag(ETCS::Buffer("Drawable2D")),
              "a leaf carries its family markers");
        check(!born->removeTag(ETCS::Buffer("Drawable2D")),
              "a family marker refuses removal -- it is a capability, not a relation");
        check(born->hasTag(ETCS::Buffer("Drawable2D")),
              "...and is still there afterwards");
        check(born->getInterfacePointer(ETCS::Buffer("Drawable2D")) != nullptr,
              "...which is what the interface pointer said all along");

        born->getOwningArena().deleteEntity(born, true);
        ETCS::MemoryArena::getInstance().deleteEntity(parent, true);
    }

    // RELEASE. A type letting go of what it held is a state change whether or
    // not it goes on to leave the tree, so it marks from the entity itself
    // rather than from its parent.
    {
        PixelNode* watcher = arena.allocate<PixelNode>();
        watcher->Allocate(2, 2);
        ReleasableNode* r = watcher->addTag<ReleasableNode>();

        ETCS::ObserverEdge e = r->Observe(watcher->getRID());
        (void)r->TakeObserved(e);
        check(!r->TakeObserved(e), "the observer of a live node settles");

        check(r->Release(), "Release runs once");
        check(r->released_ran, "...and reached the leaf's own body");
        check(r->TakeObserved(e),
              "releasing wakes whatever observes the released node");

        r->getOwningArena().deleteEntity(r, true);
        ETCS::MemoryArena::getInstance().deleteEntity(watcher, true);
    }

    // HALT, recorded the way every other state transition is: as a tag. That
    // is what makes it observable without ThreadedBase referring to Observable
    // at all -- addTag goes through tagModifyImpl, which marks.
    {
        PixelNode* watcher = arena.allocate<PixelNode>();
        watcher->Allocate(2, 2);
        ReleasableNode* h = watcher->addTag<ReleasableNode>();

        ETCS::ObserverEdge e = h->Observe(watcher->getRID());
        (void)h->TakeObserved(e);
        check(!h->hasTag(ETCS::Buffer("halted")),
              "a running body carries no halt request");

        check(h->Halt(), "the first Halt places the request");
        check(h->Halted(), "the latch answers the body's poll");
        check(h->hasTag(ETCS::Buffer("halted")),
              "...and the request is ON THE TAG SURFACE, not beside it");
        check(h->TakeObserved(e),
              "halting wakes whatever observes the halted node");

        check(!h->Halt(), "a second Halt finds one already standing");

        h->getOwningArena().deleteEntity(h, true);
        ETCS::MemoryArena::getInstance().deleteEntity(watcher, true);
    }

    // -- 13b. The edge handle: addressing without a search -------------------
    {
        PixelNode* src = arena.allocate<PixelNode>();
        src->Allocate(4, 4);

        const uint64_t devA = 7001, devB = 7002;
        ETCS::ObserverEdge ea = src->Observe(devA);
        ETCS::ObserverEdge eb = src->Observe(devB);
        check(ea.valid() && eb.valid(), "Observe hands back the observer's end of the edge");
        check(ea.slot != eb.slot, "distinct observers get distinct slots");
        check(ea.observer_rid == devA, "and the RID stays the identity the slot caches");

        check(src->TakeObserved(ea), "a fresh edge is set -- that is its default state");
        check(!src->TakeObserved(ea), "...and reading it clears that one edge");
        check(src->TakeObserved(eb), "...leaving the other observer's edge untouched");

        // Re-registering is idempotent and returns the SAME edge, not a second one.
        ETCS::ObserverEdge again = src->Observe(devA);
        check(again.slot == ea.slot, "re-observing hands back the edge already held");

        // A handle into a vacated slot verifies rather than aliasing a stranger.
        src->Unobserve(devA);
        check(src->TakeObserved(ea),
              "a stale handle reports true -- no live edge, so no claim to be current");
        check(!src->Observed() == false, "the other observer is still watching");

        ETCS::MemoryArena::getInstance().deleteEntity(src, true);
    }

    // -- 14. Observable: parent/child edges are intrinsic -------------------
    {
        PixelNode* parent = arena.allocate<PixelNode>();
        parent->Allocate(16, 16);
        PixelNode* child = parent->addTag<PixelNode>();
        child->Allocate(4, 4);

        const uint64_t watcher = 2001;
        parent->Observe(watcher);
        (void)parent->TakeObserved(watcher);          // settle

        child->FillRect(0, 0, 2, 2, 1.f, 0.f, 1.f, 1.f);
        check(parent->TakeObserved(watcher),
              "a change below bubbles up with NOTHING registered between them");

        ETCS::MemoryArena::getInstance().deleteEntity(parent, true);
    }

    // -- 15. Observable: a tag change is a state transition ----------------
    {
        PixelNode* node = arena.allocate<PixelNode>();
        node->Allocate(4, 4);
        const uint64_t watcher = 3001;
        node->Observe(watcher);
        (void)node->TakeObserved(watcher);            // settle

        check(node->addTag(ETCS::Buffer("hot")), "adding a new flag reports the transition");
        check(node->TakeObserved(watcher), "...and marks observers: tags ARE the state surface");

        check(!node->addTag(ETCS::Buffer("hot")), "re-adding a present flag moves nothing");
        check(!node->TakeObserved(watcher), "...so it must not wake anyone");

        check(node->removeTag(ETCS::Buffer("hot")), "removing it is a transition too");
        check(node->TakeObserved(watcher), "...and marks");

        check(!node->removeTag(ETCS::Buffer("hot")), "removing an absent flag moves nothing");
        check(!node->TakeObserved(watcher), "...so it must not wake anyone either");

        ETCS::MemoryArena::getInstance().deleteEntity(node, true);
    }

    // -- 16. Resizable is a pull: FollowResize builds an EDGE ---------------
    //
    // The delivery half cannot be exercised here: PollResize resolves its
    // source by RID (a follower outlives its source, so a captured pointer is
    // the bug ontology/Resizable.h documents), and a type allocated straight
    // out of the arena in this translation unit is not in the module family
    // lists that resolve reads -- probed: MISS. The OLD FollowResize resolved
    // at fire time for the same reason and would miss identically, so this is
    // the tester's reach, not a change in behaviour. The full pull is covered
    // where entities are loader-spawned (VulkanSurface follows GLFWWindow).
    //
    // What IS checkable here is the part that used to be a callback list: that
    // following registers an observation edge on the SOURCE, and that a resize
    // marks along it.
    {
        PixelNode* source   = arena.allocate<PixelNode>();
        PixelNode* follower = arena.allocate<PixelNode>();
        source->Allocate(100, 50);
        follower->Allocate(10, 10);

        check(!source->Observed(), "a source nobody follows has no observers");
        follower->FollowResize(source);
        check(source->Observed(), "FollowResize registers on the SOURCE, where the size lives");

        check(source->TakeObserved(follower->getRID()),
              "the follower starts dirty -- this is what the initial layout rides on");
        check(!source->TakeObserved(follower->getRID()), "...and settles");

        // Sixty events during a drag are sixty marks and ONE read of the last
        // size. Coalescing by construction, which the push needed a staging
        // slot and a mutex to approximate.
        source->ResizeTo(WindowSize{200, 80});
        source->ResizeTo(WindowSize{300, 90});
        source->ResizeTo(WindowSize{400, 120});
        check(source->TakeObserved(follower->getRID()),
              "three resizes wake the follower");
        check(!source->TakeObserved(follower->getRID()),
              "...ONCE -- the bit coalesces where a callback list would replay three");
        check(source->GetSize().width == 400 && source->GetSize().height == 120,
              "and the size it would read is the latest, not an intermediate one");

        ETCS::MemoryArena::getInstance().deleteEntity(follower, true);
        ETCS::MemoryArena::getInstance().deleteEntity(source, true);
    }

    // -- 17. Pushed delivery defers until the size SETTLES ------------------
    //
    // The follower here has no clock, so it cannot ask; the source wakes it
    // instead. What makes that safe is that the wake carries nothing -- the
    // follower still reads the size, so a late wake reads the current value.
    // Which in turn is what makes DEFERRING the wake free.
    {
        PixelNode* source   = arena.allocate<PixelNode>();
        PixelNode* follower = arena.allocate<PixelNode>();
        source->Allocate(100, 50);
        follower->Allocate(10, 10);

        follower->FollowResize(source, ResizeDelivery::Pushed);
        check(!source->settleResize(), "an idle source counts down nothing");

        source->deliverSize(WindowSize{200, 80});
        for (int i = 1; i < RESIZE_SETTLE_FRAMES; ++i)
            check(!source->settleResize(), "a resize does not wake on the same pass it arrives");
        check(source->settleResize(), "...it wakes once the pump has been quiet");
        check(!source->settleResize(), "...and exactly once");

        // A drag: every event re-arms, so the wake is deferred for as long as
        // it lasts. This is the whole difference from a rate limit, which
        // would have fired mid-drag on a size already superseded.
        for (int i = 0; i < 20; ++i)
        {
            source->deliverSize(WindowSize{ 200u + i, 80u });
            check(!source->settleResize(), "a continuing drag keeps deferring");
        }
        // Bounded rather than exact: the loop above already spent passes of
        // the countdown, and the claim under test is "it fires once the pump
        // goes quiet", not which pass it lands on.
        int woke = 0;
        for (int i = 0; i < RESIZE_SETTLE_FRAMES; ++i) if (source->settleResize()) ++woke;
        check(woke == 1, "one wake for the whole drag, when it stops");
        check(source->recordedSize().width == 219,
              "and the size waiting to be read is where the drag ENDED");

        ETCS::MemoryArena::getInstance().deleteEntity(follower, true);
        ETCS::MemoryArena::getInstance().deleteEntity(source, true);
    }

    // -- 18. Polled delivery asks for no wake ------------------------------
    {
        PixelNode* source   = arena.allocate<PixelNode>();
        PixelNode* follower = arena.allocate<PixelNode>();
        source->Allocate(64, 64);
        follower->Allocate(8, 8);

        follower->FollowResize(source);          // Polled is the default
        source->deliverSize(WindowSize{128, 128});
        for (int i = 0; i < RESIZE_SETTLE_FRAMES + 2; ++i)
            check(!source->settleResize(),
                  "a polled follower arms no countdown -- it will ask on its own tick");
        check(source->TakeObserved(follower->getRID()),
              "the EDGE is identical either way; only the delivery differs");

        ETCS::MemoryArena::getInstance().deleteEntity(follower, true);
        ETCS::MemoryArena::getInstance().deleteEntity(source, true);
    }

    // -- 19. Thread: the actor, and the DAG as the parent edge --------------
    {
        Worker* root = arena.allocate<Worker>();
        root->script = ETCS::Buffer("root.etcs");

        check(root->getInterfacePointer(ETCS::Buffer("Thread")) != nullptr,
              "leaf registers Thread");
        check(root->getInterfacePointer(ETCS::Buffer("Threaded")) != nullptr,
              "leaf registers Threaded (inherited lineage) -- one cannot be held without the other");
        check(root->Script() == ETCS::Buffer("root.etcs"), "Script dispatches to the leaf");

        const ETCS::RID a = root->Detach(ETCS::Buffer("a.etcs"));
        const ETCS::RID b = root->Detach(ETCS::Buffer("b.etcs"));
        check(a != 0 && b != 0 && a != b, "detaching yields distinct child RIDs");

        // "Which jobs did this one start" is the child registry, not a
        // registry beside it -- the whole DetachedRegistry, answered.
        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> kids;
        root->getOrderedTypedChildren(kids);
        check(kids.size() == 2, "the detached jobs ARE this thread's children");

        ETCS::MemoryArena::getInstance().deleteEntity(root, true);
    }

    // -- 20. Thread: a halted actor spawns nothing -------------------------
    {
        Worker* w = arena.allocate<Worker>();
        check(w->Halt(), "the first Halt places the request");
        check(!w->Halt(), "...a second finds one already standing");
        check(w->Halted(), "and the body's poll sees it");
        check(w->Detach(ETCS::Buffer("late.etcs")) == 0,
              "a halted thread refuses to detach -- 0 rather than a silent no-op");
        ETCS::MemoryArena::getInstance().deleteEntity(w, true);
    }

    // -- 20b. The closure boundary ----------------------------------------
    //
    // raiseClosure means "end the closure this call belongs to", and before a
    // boundary existed it could not: every chain is pinned to
    // RootSignalContext() by construction and the root's interrupt IS
    // g_sig_int, so the outermost authority had one reachable value and every
    // closure raise from anywhere was a process-wide SIGINT. Clicking a
    // window's X ended the runtime.
    //
    // g_sig_int IS ASSERTED UNTOUCHED, not merely unread. That is the whole
    // claim, and a test that only checked the Thread's own flag would pass
    // just as well on the broken version -- both flags end up raised there.
    {
        check(g_sig_int.load(std::memory_order_acquire) == 0,
              "the process interrupt starts clear");

        // A Worker, because it claims Thread. ReleasableNode claims only
        // Threaded, whose Signals() is the refusing default -- owning a body
        // is not being an actor, and only an actor bounds a closure.
        Worker* boundary = arena.allocate<Worker>();
        ETCS::SignalContext shell = boundary->Signals();
        check(shell.closure_root, "a Thread's context IS a closure boundary");
        check(shell.interrupt != nullptr,
              "...and holds real interrupt authority to end it with");
        ETCS::SignalContext threaded_only = ETCS::SignalContext{};
        {
            ReleasableNode* merely_threaded = arena.allocate<ReleasableNode>();
            threaded_only = merely_threaded->Signals();
            check(!threaded_only.closure_root,
                  "a Threaded that is not a Thread bounds nothing -- it owns a body, "
                  "it is not an actor");
            merely_threaded->getOwningArena().deleteEntity(merely_threaded, true);
        }
        if (!shell.interrupt) { check(false, "cannot continue without authority"); return g_fail; }

        // A call nested inside that closure, the shape a script line has.
        ETCS::SignalFlag inner_flag{0};
        ETCS::SignalContext inner;
        inner.interrupt = &inner_flag;
        inner.setParent(&shell);

        check(inner.raiseClosureInterrupt(),
              "a raise inside the closure finds an authority");
        check(shell.interrupt->load(std::memory_order_acquire) != 0,
              "...and it is the BOUNDARY's, not the call's own");
        check(inner_flag.load(std::memory_order_acquire) == 0,
              "...not the nearest one -- nearest is the call this verb exists not to end");
        check(g_sig_int.load(std::memory_order_acquire) == 0,
              "...and the process interrupt is UNTOUCHED -- the closure ended, not the runtime");

        // Reads cross the boundary in both directions: bounding who a raise
        // REACHES must not bound who hears one, or a real SIGTERM would stop
        // at the same edge.
        check(inner.isInterrupted(),
              "everything inside the closure still hears the raise");

        // Outside any closure, the old behaviour is the right one and stands.
        ETCS::SignalContext loose;
        loose.setParent(&ETCS::RootSignalContext());
        check(loose.closureRoot() == nullptr, "a chain with no boundary reports none");
        check(inner.closureRoot() == &shell, "...and one with a boundary names it");

        shell.interrupt->store(0, std::memory_order_release);
        boundary->getOwningArena().deleteEntity(boundary, true);
        check(g_sig_int.load(std::memory_order_acquire) == 0,
              "the process interrupt is still clear when this test is done");
    }

    // -- 21. Thread: the closure travels by value --------------------------
    //
    // The reason it is family state and not a local: a detached job outlives
    // the statement that launched it, so by the time it runs the only surviving
    // reference to its inputs is the copy it carries.
    {
        Worker* parent = arena.allocate<Worker>();
        const char* q = "SELECT * FROM t";
        check(parent->Bind(ETCS::Buffer("query"), q, std::strlen(q)),
              "a binding that fits is taken");
        check(parent->ClosureSize() == 1, "and lands in the closure");

        ETCS::Buffer got;
        check(parent->Lookup(ETCS::Buffer("query"), got) && got == ETCS::Buffer(q),
              "and reads back verbatim");

        const ETCS::RID kid = parent->Detach(ETCS::Buffer("run_query.etcs"));
        // getTrueType, not static_cast: Entity is a virtual base, so
        // base-to-derived is illegal (addTagTrampoline's own comment says why).
        ETCS::Entity* kid_e = parent->getTypedChild(ETCS::Buffer("Worker"), kid);
        Worker* child = kid_e ? static_cast<Worker*>(kid_e->getTrueType()) : nullptr;
        check(child != nullptr, "the child exists");
        ETCS::Buffer inherited;
        check(child && child->Lookup(ETCS::Buffer("query"), inherited)
                    && inherited == ETCS::Buffer(q),
              "the child carries a COPY of the closure into its own lifetime");

        // Rebinding the parent must not reach into a job already launched.
        const char* q2 = "DROP TABLE t";
        parent->Bind(ETCS::Buffer("query"), q2, std::strlen(q2));
        ETCS::Buffer after;
        child->Lookup(ETCS::Buffer("query"), after);
        check(after == ETCS::Buffer(q),
              "...a copy, not a view -- the parent rebinding does not rewrite a running job");

        // Too large to fit fails AT THE BINDING rather than truncating.
        std::string huge(ETCS::Buffer::bufsize + 16, 'x');
        check(!parent->Bind(ETCS::Buffer("huge"), huge.c_str(), huge.size()),
              "a blob too large for the buffer is refused, not silently shortened");
        check(parent->ClosureSize() == 1, "...and nothing is bound as a side effect");

        ETCS::MemoryArena::getInstance().deleteEntity(parent, true);
    }

    // -- 22. Observable: the containment edge runs BOTH ways -----------------
    //
    // Upward and downward are different statements about the same edge, and the
    // whole point of separating them is that neither implies the other.
    {
        PixelNode* parent = arena.allocate<PixelNode>();
        parent->Allocate(16, 16);
        PixelNode* child = parent->addTag<PixelNode>();
        child->Allocate(4, 4);
        PixelNode* grandchild = child->addTag<PixelNode>();
        grandchild->Allocate(2, 2);

        const uint64_t above = 4001;
        parent->Observe(above);
        child->ObserveSelf();
        grandchild->ObserveSelf();
        (void)parent->TakeObserved(above);
        (void)child->TakeObserved(child->getRID());
        (void)grandchild->TakeObserved(grandchild->getRID());

        // Downward: the frame the children sit in moved.
        parent->MarkObservedBelow();
        check(child->TakeObserved(child->getRID()),
              "a downward mark reaches the direct child");
        check(!parent->TakeObserved(above),
              "...and does NOT travel up -- moving is not the same as changing content");
        check(!grandchild->TakeObserved(grandchild->getRID()),
              "...nor past one level: the rest is reached by composition, as upward is");

        // The child continuing the statement is what carries it the rest of the way.
        child->MarkObservedBelow();
        check(grandchild->TakeObserved(grandchild->getRID()),
              "a child that acts on the mark passes it on, and the subtree is covered");

        // Upward is unchanged, and still does not leak downward.
        (void)child->TakeObserved(child->getRID());
        grandchild->FillRect(0, 0, 1, 1, 1.f, 0.f, 0.f, 1.f);
        check(parent->TakeObserved(above),
              "a change below still bubbles to an observer above");
        check(child->TakeObserved(child->getRID()),
              "...marking the compositors on the way, which is the upward path");

        ETCS::MemoryArena::getInstance().deleteEntity(parent, true);
    }

    // -- 23. Observable: MarkObservedLocal is the primitive, and it stays put --
    {
        PixelNode* parent = arena.allocate<PixelNode>();
        parent->Allocate(8, 8);
        PixelNode* child = parent->addTag<PixelNode>();
        child->Allocate(2, 2);

        const uint64_t above = 5001;
        parent->Observe(above);
        child->ObserveSelf();
        (void)parent->TakeObserved(above);
        (void)child->TakeObserved(child->getRID());

        child->MarkObservedLocal(0);
        check(child->TakeObserved(child->getRID()), "a local mark marks my own observers");
        check(!parent->TakeObserved(above), "...and walks nowhere at all");

        ETCS::MemoryArena::getInstance().deleteEntity(parent, true);
    }

    // -- 24. A concurrent foreign mark survives a node's own write ----------
    //
    // The lost update the origin exists to prevent, kept as a test because it
    // was real: with a bare flag, a node recomposing marked ITSELF along with
    // everyone else, so it cleared its own bit afterwards -- and that clear
    // could not tell its own mark from one a concurrent writer had left during
    // the write. This is that exact interleaving.
    {
        PixelNode* node = arena.allocate<PixelNode>();
        node->Allocate(4, 4);
        node->ObserveSelf();
        (void)node->TakeObserved(node->getRID());          // the gate settles it

        node->FillRect(0, 0, 2, 2, 1.f, 0.f, 0.f, 1.f);    // my own write, mid-recompose
        check(!node->TakeObserved(node->getRID()),
              "my own write does not mark my own edge");

        // Meanwhile another thread changes a child, which bubbles to me. Under
        // the old shape this and the line above were indistinguishable.
        node->MarkObservedLocal(0);                         // origin: not me
        node->FillRect(0, 0, 1, 1, 0.f, 1.f, 0.f, 1.f);    // recompose keeps writing

        check(node->TakeObserved(node->getRID()),
              "a foreign mark arriving during my own write SURVIVES it");

        ETCS::MemoryArena::getInstance().deleteEntity(node, true);
    }

    // -- 25. The chain: capacity past one word, origin exact at depth --------
    //
    // Extension blocks are STORAGE, not observers -- this entity's slot table
    // continued elsewhere, reached by pointer and never addressed as an edge.
    // So no bits are reserved for them, marks propagate into them eagerly
    // carrying the original origin, and an RID being a global identity means
    // depth changes where a slot lives, never what the edge is.
    {
        PixelNode* src = arena.allocate<PixelNode>();
        src->Allocate(4, 4);

        // One past a single word, so at least one extension block is forced.
        const unsigned N = ETCS_OBSERVABLE_EDGE_BITS + 8;
        std::vector<ETCS::ObserverEdge> edges;
        for (unsigned k = 0; k < N; ++k)
            edges.push_back(src->Observe(9000 + k));

        bool all_valid = true, spans_blocks = false;
        for (const auto& e : edges)
        {
            if (!e.valid()) all_valid = false;
            if (e.slot >= ETCS_OBSERVABLE_EDGE_BITS) spans_blocks = true;
        }
        check(all_valid, "capacity extends past one word");
        check(spans_blocks, "...into a second block, by index");
        check(src->Observed(), "and the entity reports as watched");

        for (const auto& e : edges) (void)src->TakeObserved(e);   // settle every edge

        // A change caused by ONE observer must reach all the others and skip
        // only that one -- including when cause and observer are in different
        // blocks, which is the case the chain could have blurred.
        const uint64_t cause = 9000 + (ETCS_OBSERVABLE_EDGE_BITS + 2);   // in block 1
        src->MarkObservedLocal(cause);

        unsigned told = 0, cause_told = 0;
        for (const auto& e : edges)
        {
            const bool dirty = src->TakeObserved(e);
            if (e.observer_rid == cause) cause_told += dirty ? 1 : 0;
            else                          told      += dirty ? 1 : 0;
        }
        check(told == N - 1, "every other edge is told, across both blocks");
        check(cause_told == 0, "and the cause is not told about its own change, at depth");

        // The edge in the far block still round-trips by RID as well as handle.
        const uint64_t far = 9000 + ETCS_OBSERVABLE_EDGE_BITS;
        src->MarkObservedLocal(0);
        check(src->TakeObserved(far), "an edge in an extension block reads by RID too");
        check(!src->TakeObserved(far), "...and clears just that one");

        ETCS::MemoryArena::getInstance().deleteEntity(src, true);
    }

    // -- 26. A reclaimed Thread's signal bits are not handed to the next -----
    //
    // A SignalContext is copied and carried -- a detached job holds one for as
    // long as it runs -- so the bits it points at must outlive the entity that
    // published them. Entity members cannot: reclaimEntity memsets the whole
    // outer shell and pushes it onto the exact-(size, alignment) free list,
    // where the next allocate of the SAME concrete type takes it. A held copy
    // then addresses a DIFFERENT LIVE THREAD's stop flag -- not a crash, a
    // silent misrouting, the hazard SignalContext::provider documents for
    // itself.
    //
    // MEASURED, not argued: with the flags as entity members this reports
    // "shell reused: yes, signal bits aliased: YES" on the first retry. The
    // flags come from the root arena instead, whose memory is released only at
    // module unload -- which cannot happen while anything holds the lifetime
    // token, so a flag always outlives every reader of it.
    {
        Worker* first = arena.allocate<Worker>();
        ETCS::SignalContext stale = first->Signals();
        void* first_addr = static_cast<void*>(first);
        ETCS::MemoryArena::getInstance().deleteEntity(first, true);

        // Several, because the free list need not hand the very next allocation
        // the reclaimed shell. One alias anywhere is the bug.
        bool reused = false, aliased = false;
        std::vector<Worker*> later;
        for (int i = 0; i < 8; ++i)
        {
            Worker* w = arena.allocate<Worker>();
            if (!w) break;
            later.push_back(w);
            if (static_cast<void*>(w) == first_addr) reused = true;
            if (w->Signals().terminate == stale.terminate) aliased = true;
        }

        // Reported so a future reader can tell a PASS from a vacuous one: if the
        // shell is never reused, this test is not exercising anything.
        std::cout << "    [note] reclaimed shell reused: " << (reused ? "yes" : "no") << "\n";
        check(reused, "the reclaimed shell IS recycled -- so this test is not vacuous");
        check(!aliased, "a reclaimed Thread's signal bits are handed to no later Thread");

        if (stale.terminate)
        {
            *stale.terminate = 1;
            bool any = false;
            for (Worker* w : later) if (w->Signals().isTerminated()) any = true;
            check(!any, "so a stale copy from a dead job terminates no live one");
            *stale.terminate = 0;
        }
        for (Worker* w : later) ETCS::MemoryArena::getInstance().deleteEntity(w, true);
    }

    /*
     * -- 27. The raster split: one question about size, two about storage ----
     *
     * Pixels used to be asked two different things at once. "How big is this"
     * and "where are its bytes" have the same answer only while every raster
     * in the system is CPU-backed, and RenderProvider had three consumers
     * relying on that: two coordinate-origin walks asking for "Pixels" to mean
     * "is this node a raster", and a blit path that refused everything without
     * host bytes with one message covering two unrelated causes.
     *
     * Raster is the first question. Pixels and Renderable are the second, and
     * they are mutually exclusive by composition -- see DeviceNode above for
     * why that is a compile error rather than anything asserted here.
     */
    {
        PixelNode* cpu = ETCS::MemoryArena::getInstance().allocate<PixelNode>();
        DeviceNode* gpu = ETCS::MemoryArena::getInstance().allocate<DeviceNode>();
        check(cpu && gpu, "a host-resident node and a device-resident one");

        if (cpu && gpu)
        {
            cpu->Allocate(64, 32);
            gpu->device = 0xD1CE; gpu->dw = 128; gpu->dh = 96;

            Raster_* rc = static_cast<Raster_*>(cpu->getInterfacePointer(ETCS::Buffer("Raster")));
            Raster_* rg = static_cast<Raster_*>(gpu->getInterfacePointer(ETCS::Buffer("Raster")));

            check(rc && rg,
                  "BOTH answer \"Raster\" -- the size question does not depend on "
                  "whose memory the answer lives in");
            check(rc && rc->PixelWidth() == 64 && rc->PixelHeight() == 32,
                  "a CPU raster's size is its buffer's, answered through Raster");
            check(rg && rg->PixelWidth() == 128 && rg->PixelHeight() == 96,
                  "a device raster's size is the device image's, answered the same way");

            // The disjointness, from both sides. Not the exclusivity itself --
            // that is the compile -- but the thing every consumer branches on.
            check(cpu->getInterfacePointer(ETCS::Buffer("Pixels")) != nullptr
               && cpu->getInterfacePointer(ETCS::Buffer("Renderable")) == nullptr,
                  "a host raster is Pixels and not Renderable");
            check(gpu->getInterfacePointer(ETCS::Buffer("Renderable")) != nullptr
               && gpu->getInterfacePointer(ETCS::Buffer("Pixels")) == nullptr,
                  "a device raster is Renderable and not Pixels -- so \"no Pixels\" "
                  "is a statement about locality, not about being a picture");

            // The origin walk's question, in the form it is actually asked.
            // Answering "Pixels" here was the bug: a device-resident drawable
            // ancestor is a coordinate origin and was walked straight past.
            check(gpu->getInterfacePointer(ETCS::Buffer("Drawable2D")) != nullptr
               && gpu->getInterfacePointer(ETCS::Buffer("Raster")) != nullptr,
                  "a device-resident DRAWABLE stops an origin walk, because the walk "
                  "asks Raster");

            // Empty is "not sized yet", which is a real state and not an error.
            DeviceNode* unborn = ETCS::MemoryArena::getInstance().allocate<DeviceNode>();
            if (unborn)
            {
                Raster_* ru = static_cast<Raster_*>(
                    unborn->getInterfacePointer(ETCS::Buffer("Raster")));
                check(ru && ru->RasterEmpty(),
                      "a raster with no size yet reports empty rather than lying about one");
                check(rg && !rg->RasterEmpty(),
                      "...and a sized one does not");

                // DeviceKey's only defined operation. Equal and non-zero is
                // the one case a device-to-device copy would be legal in;
                // zero is "no device yet", which must never compare equal to
                // a real one just because both sides are unset.
                Renderable_* nu = static_cast<Renderable_*>(
                    unborn->getInterfacePointer(ETCS::Buffer("Renderable")));
                Renderable_* ng = static_cast<Renderable_*>(
                    gpu->getInterfacePointer(ETCS::Buffer("Renderable")));
                check(nu && nu->DeviceKey() == 0,
                      "an uncreated device raster names no device");
                check(ng && nu && ng->DeviceKey() != nu->DeviceKey(),
                      "and does not read as being on the same device as a created one");

                DeviceNode* sibling = ETCS::MemoryArena::getInstance().allocate<DeviceNode>();
                if (sibling)
                {
                    sibling->device = gpu->device;
                    Renderable_* ns = static_cast<Renderable_*>(
                        sibling->getInterfacePointer(ETCS::Buffer("Renderable")));
                    check(ns && ng && ns->DeviceKey() == ng->DeviceKey(),
                          "two rasters on one device say so, which is the whole of what "
                          "DeviceKey is for");
                    ETCS::MemoryArena::getInstance().deleteEntity(sibling, true);
                }
                ETCS::MemoryArena::getInstance().deleteEntity(unborn, true);
            }

            ETCS::MemoryArena::getInstance().deleteEntity(gpu, true);
            ETCS::MemoryArena::getInstance().deleteEntity(cpu, true);
        }
    }

    /*
     * -- 28. Device: capability as structure, and the toggle it enables -----
     *
     * THERE IS ALWAYS A CPU, so host projection is the floor and needs no
     * declaration; a device is strictly an addition, and in this ontology an
     * addition is a child. That makes "can this camera reach a device" a fact
     * about the entity graph rather than a flag beside it -- enumerable by the
     * same typed-child walk everything else uses, and true or false for the
     * same reason anything else here is.
     *
     * The toggle stores only the REQUEST. The effective mode is that request
     * AND a device still being there, derived at read time, so the two cannot
     * disagree -- which is what makes losing the device a silent, correct
     * fallback instead of a state to repair.
     */
    {
        TestCamera* cam = ETCS::MemoryArena::getInstance().allocate<TestCamera>();
        check(cam != nullptr, "a camera with no device under it");

        if (cam)
        {
            check(cam->DeviceSource() == nullptr,
                  "a camera with no Device child reaches no device");
            check(cam->DeviceProjectionRequested(),
                  "a camera WANTS a device by default -- attaching one is meant to "
                  "be the whole of switching");
            check(!cam->DeviceProjection(),
                  "...and is still on the host, because wanting one is not having "
                  "one: there is always a CPU and it needed no declaring");

            TestDevice* dev = cam->addTag<TestDevice>();
            check(dev != nullptr, "a Device spawned as a child of the camera");

            if (dev)
            {
                // Declared but not ready: the child exists, the device does not.
                check(cam->DeviceSource() == nullptr,
                      "an uncreated Device is not a device the camera can reach -- "
                      "declared and usable are different answers");

                dev->key = 0xD1CE;
                check(cam->DeviceSource() == dev,
                      "once ready, the child IS the capability -- found by the "
                      "ordinary typed-child walk, with nothing registered beside it");

                check(cam->DeviceProjection(),
                      "AND THE CAMERA IS ON IT, with nothing else said -- attaching "
                      "the capability is what switches, so a device that is present "
                      "and unused is not a state this can be in");

                // The override, which is the only thing the toggle is for now.
                cam->SetDeviceProjection(false);
                check(cam->DeviceSource() != nullptr && !cam->DeviceProjection(),
                      "and it can still be held on the host deliberately -- the "
                      "device is reachable, the camera just is not using it");
                cam->SetDeviceProjection(true);
                check(cam->DeviceProjection(), "...and handed back");

                // WHICH device is the same mechanism as WHETHER -- a second
                // child is a second device available, no new concept.
                TestDevice* dev2 = cam->addTag<TestDevice>();
                if (dev2)
                {
                    dev2->key = 0xBEEF;
                    check(cam->DeviceSource() != nullptr,
                          "two Devices under one camera is two devices available to it");
                    // Through its OWN arena: a child made by addTag<T> lives in
                    // its parent's, and the singleton's deleteEntity is a silent
                    // no-op on anything that is not its own.
                    dev2->getOwningArena().deleteEntity(dev2, true);
                }

                // AND THE FALLBACK IS FREE. Take the device away with the
                // request still standing.
                dev->key = 0;
                check(cam->DeviceProjectionRequested(),
                      "the request survives the device going away -- it is what "
                      "was asked, not what is happening");
                check(!cam->DeviceProjection(),
                      "...but the camera is back on the host, with nothing to reset: "
                      "there is always a CPU");

                dev->getOwningArena().deleteEntity(dev, true);
                check(cam->DeviceSource() == nullptr && !cam->DeviceProjection(),
                      "and deleting the child is the same answer by the same route");
            }
            ETCS::MemoryArena::getInstance().deleteEntity(cam, true);
        }
    }

    /*
     * -- 29. Halting is a transition; stopped is the destination -------------
     *
     * One tag was carrying both. "halted" was written when a stop was ASKED
     * FOR, so an entity mid-wind-down and an entity whose loop had actually
     * left were indistinguishable on the state surface -- and the tag's name
     * claimed the second while recording the first.
     *
     * Halt is a request from outside; Stop is a notification from the body,
     * the same split Delete/Release already draws (ontology/Lifecycle.h). Only
     * the body knows when its loop has gone, so only the body can say it.
     */
    {
        Worker* w = ETCS::MemoryArena::getInstance().allocate<Worker>();
        check(w != nullptr, "an actor with a body to stop");

        if (w)
        {
            check(!w->Halted() && !w->Stopped(),
                  "a fresh actor is neither asked to stop nor stopped");
            check(!w->hasTag(ETCS::Buffer("halted")) && !w->hasTag(ETCS::Buffer("stopped")),
                  "...and says neither on its tag surface");

            check(w->Halt(), "the halt is taken by the caller that placed it");
            check(w->Halted(), "the request stands");
            check(w->hasTag(ETCS::Buffer("halted")),
                  "and it is ON THE TAG SURFACE, which is what makes it observable "
                  "with no Observable code in the halt path at all");
            check(!w->Stopped() && !w->hasTag(ETCS::Buffer("stopped")),
                  "BUT IT HAS NOT STOPPED -- being asked and having gone are "
                  "different facts, and this is the one that used to be unsayable");

            check(w->Stop(), "the body reports that it has left");
            check(w->Stopped(), "so the destination is reached");
            check(w->hasTag(ETCS::Buffer("stopped")),
                  "and recorded where the request was");
            check(!w->hasTag(ETCS::Buffer("halted")),
                  "REPLACING the request rather than sitting beside it: carrying "
                  "both would claim to be winding down and finished at once");

            check(!w->Stop(), "a second Stop is not a second transition");
            check(!w->Halt(), "and the halt was already standing");
            check(w->Halted() && w->Stopped(),
                  "both latches still read true -- a halt is one-way, and so is "
                  "leaving");

            ETCS::MemoryArena::getInstance().deleteEntity(w, true);
        }

        // A body that finishes its own work was never asked to stop, and is no
        // less stopped for it. Removing a tag that was never written is a
        // no-op, so this path costs a lookup and says the true thing.
        Worker* q = ETCS::MemoryArena::getInstance().allocate<Worker>();
        if (q)
        {
            check(q->Stop() && q->Stopped(),
                  "a body can stop without ever having been asked");
            check(q->hasTag(ETCS::Buffer("stopped")) && !q->Halted(),
                  "...and says so without claiming a request that never happened");
            ETCS::MemoryArena::getInstance().deleteEntity(q, true);
        }
    }

    /*
     * -- 31. Matrix: the shape is the constraint, twice ----------------------
     *
     * An n-by-m may feed an m-by-p and nothing else. That rule is stated at two
     * different TIMES, and neither is a weaker copy of the other:
     *
     *   COMPILE TIME  Mat<R,C>::operator* takes a Mat<C,P>, so a mismatched
     *                 product does not bind. Concrete code cannot write one.
     *   RESOLVE TIME  ChainsInto compares a stage's columns to the next
     *                 stage's rows. A chain assembled by ATTACHING children is
     *                 not knowable statically, so this is the only form
     *                 available where the chain is actually built.
     *
     * And the family owns the VALUE, not just the constraint -- m-by-n
     * elements, the same division Raster_ makes for its buffer. A stage whose
     * values lived elsewhere could disagree with its own shape, and the shape
     * is the only thing the chain checks.
     */
    {
        // The value type first: the product's shape is carried by the type.
        Matrix4 id = Matrix4::Identity();
        check(id.at(0,0) == 1.0f && id.at(1,1) == 1.0f && id.at(0,1) == 0.0f,
              "Mat<4,4>::Identity is the identity");

        Matrix4 t = Matrix4::Identity();
        t.at(0,3) = 5.0f; t.at(1,3) = -2.0f;
        const Matrix4 once = id * t;
        check(once == t, "identity times a translation is that translation");

        // n-by-m times m-by-p yields n-by-p, and the SHAPE comes out in the
        // type -- this would not compile if the inner dimensions disagreed.
        Mat<2,3> a{}; Mat<3,4> b{};
        a.at(0,0)=1; a.at(0,1)=2; a.at(0,2)=3;
        a.at(1,0)=4; a.at(1,1)=5; a.at(1,2)=6;
        for (uint32_t i=0;i<3;++i) b.at(i,i)=1.0f;
        Mat<2,4> ab = a * b;
        check(Mat<2,4>::rows == 2 && Mat<2,4>::cols == 4,
              "a 2x3 times a 3x4 is a 2x4, and the type says so");
        check(ab.at(0,0)==1 && ab.at(1,2)==6 && ab.at(0,3)==0,
              "...with the product actually computed");

        // The OrderVector -> 4x4 function: DERIVED, not reinterpreted. Row 0's
        // fourth slot is a RID and no product may touch it, which is why this
        // is a function rather than a cast.
        OrderVector ov;
        ov.PlaceAt(3.0f, 4.0f, 5.0f);
        ov.rid = 12345;
        const Matrix4 om = ov.ToMatrix4();
        check(om.at(0,3)==3.0f && om.at(1,3)==4.0f && om.at(2,3)==5.0f,
              "an OrderVector's position becomes the translation column");
        check(om.at(3,3)==1.0f && om.at(3,0)==0.0f,
              "and the bottom row is affine -- the RID is nowhere in the matrix, "
              "which is the whole reason this is built rather than cast");
        check(ov.rid == 12345, "...and the vector still knows who it is");

        // Now the family: stages, their values, and the chain rule.
        Xform* first  = ETCS::MemoryArena::getInstance().allocate<Xform>();
        Xform* second = ETCS::MemoryArena::getInstance().allocate<Xform>();
        check(first && second, "two transform stages");

        if (first && second)
        {
            check(first->getInterfacePointer(ETCS::Buffer("Matrix")) != nullptr,
                  "a stage is a Matrix");
            check(first->getInterfacePointer(ETCS::Buffer("Wrapper")) != nullptr
               && first->hasTag(ETCS::Buffer("Wrapper")),
                  "AND A WRAPPER, with nothing registered here -- which is what "
                  "makes an attached stage part of a chain MirrorBuffer already "
                  "knows how to resolve and apply");

            first->Become(t);
            check(first->ShapeIs(4,4) && first->Count() == 16,
                  "loading a Mat<4,4> gives the stage that shape");
            check(first->At(0,3) == 5.0f,
                  "and the family owns the elements, not an interface to them");
            check(first->As<4,4>() == t,
                  "which round-trip through the typed bridge unchanged");
            check(first->As<2,3>() == Mat<2,3>{},
                  "asking for the wrong shape yields zero rather than a lie");

            // The chain rule.
            second->Become(Matrix4::Identity());
            check(first->ChainsInto(*second),
                  "4x4 feeds 4x4 -- my columns are the next stage's rows");
            second->BecomeShape(3, 3);
            check(!first->ChainsInto(*second),
                  "and a 4x4 does NOT feed a 3x3: the chain is dimensionally "
                  "constrained even though it was assembled at runtime");
            second->BecomeShape(4, 2);
            check(first->ChainsInto(*second) && !second->ChainsInto(*first),
                  "the relation is DIRECTED -- 4x4 into 4x2 composes, the "
                  "reverse does not, because a pipeline has a direction");

            // Wrap and Unwrap are not inverses, and nothing here says they are.
            ETCS::MBuffer io;
            ETCS::SignalContext sig{};
            first->Wrap(io, sig);
            check(first->wraps == 1 && first->unwraps == 0,
                  "a stage that multiplies on the way out does nothing on the "
                  "way in -- arriving is not undoing");

            ETCS::MemoryArena::getInstance().deleteEntity(second, true);
            ETCS::MemoryArena::getInstance().deleteEntity(first, true);
        }
    }

    ETCS::MemoryArena::getInstance().deleteEntity(canvas, true);
    ETCS::MemoryArena::getInstance().deleteEntity(sink, true);

    std::cout << "=== Summary: " << g_pass << " passed, " << g_fail << " failed ===\n";
    return g_fail == 0 ? 0 : 1;
}
