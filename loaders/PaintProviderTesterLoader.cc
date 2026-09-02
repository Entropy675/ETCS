// PaintProviderTesterLoader.cc
//
// A correctness pass over PaintProvider AND the 2D surface it is built on,
// headless and with no GPU:
//
//   ./Run_PaintProviderTesterLoader
//
// WHY THE TWO ARE ONE SUITE. PaintProvider owns no raster backend -- it
// composes RenderProvider's Surface family and adds a document model over it.
// So there is no way to test the paint side without exercising the 2D side,
// and no reason to want one: what a paint program actually depends on is that
// Clear replaces, DrawRect composites source-over, Blit lands where it is told,
// and a RID handed across a module boundary resolves to the surface it names.
// Those are the assertions below, in that order, and every one of them is made
// against BYTES rather than against "it did not crash".
//
// The canvas is a RenderProvider ImageSurface -- [Surface + Pixels + Resizable],
// CPU-backed, needing neither a window nor a Vulkan device -- reached generically
// through getInterfacePointer("Pixels"). That is the same seam a layer editor
// uses, so testing through it tests the seam too.
//
// THE ONE THAT MATTERS MOST is section 1. PaintProvider reaches its target with
// ETCS::resolve_in_family<Surface_>("Surface", rid) -- a COMPOSED reference,
// resolved by the loader through the origin-affixed key that records which
// provider owns the entity (core/Entity.h). If that resolution ever returns
// null, every stamp becomes a no-op and NOTHING reports it: the paint smoke
// script looked exactly like a working program with an invisible brush. That
// failure is silent by construction, so it needs a test that is loud.
//
// NOT COVERED, said here rather than left to be discovered: the
// PaintInput::ConsumeInput stream BODY. It needs a live edge and therefore a
// producer. The state machine that body drives IS covered -- section 5 goes
// through PaintInput.Pointer/Press/Release, which build an InputEvent and call
// the same HandleEvent the body calls per event. So what is untested is the
// wiring alone, and paint_smoke.etcs covers that.
//
// A loader reaches a module only through its published verbs, which is the
// right constraint and is why those three exist: a stroke you cannot script is
// a stroke you can only ever watch.

#include "../ETCS.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const std::string& what)
{
    if (ok) { ++g_pass; std::cout << "  ok    " << what << "\n"; }
    else    { ++g_fail; std::cout << "  FAIL  " << what << "\n"; }
}

struct RGBA { int r, g, b, a; };

static RGBA pixel_at(Pixels_* px, uint32_t x, uint32_t y)
{
    if (!px || !px->PixelData()) return { -1, -1, -1, -1 };
    if (x >= px->PixelWidth() || y >= px->PixelHeight()) return { -1, -1, -1, -1 };
    const uint8_t* p = px->PixelData() + (static_cast<size_t>(y) * px->PixelStride()) + (x * 4);
    return { p[0], p[1], p[2], p[3] };
}

// Tolerance is a parameter rather than a constant because the two paths have
// different exactness: an opaque write is byte-exact, a source-over blend is
// integer arithmetic on a float, and the subsampled layer composite lands on a
// 4px grid. Using one loose tolerance everywhere would let a real regression in
// the exact paths through.
static void check_pixel(Pixels_* px, uint32_t x, uint32_t y, RGBA want,
                        int tol, const std::string& what)
{
    const RGBA got = pixel_at(px, x, y);
    const bool ok = std::abs(got.r - want.r) <= tol && std::abs(got.g - want.g) <= tol
                 && std::abs(got.b - want.b) <= tol && std::abs(got.a - want.a) <= tol;
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << " (" << x << "," << y << ") = "
              << got.r << "," << got.g << "," << got.b << "," << got.a;
    if (!ok) std::cout << "   expected " << want.r << "," << want.g << ","
                       << want.b << "," << want.a << " +/-" << tol;
    std::cout << "\n";
    ok ? ++g_pass : ++g_fail;
}

static std::string rid_arg(ETCS::Entity* e) { return std::to_string(e->getRID()); }

int main()
{
    WIRE_CONTEXT();

    constexpr uint32_t W = 128;
    constexpr uint32_t H = 96;
    const std::string SIZE = std::to_string(W) + " " + std::to_string(H);

    ETCS::Entity* canvas = ETCS::spawn_entity("RenderProvider", "ImageSurface", env, loader);
    if (!canvas) { std::cerr << "cannot spawn RenderProvider:ImageSurface\n"; return 1; }
    canvas->call("ImageSurface.Create", SIZE.c_str(), ctx);

    Pixels_* px = static_cast<Pixels_*>(canvas->getInterfacePointer(ETCS::Buffer("Pixels")));
    if (!px || !px->PixelData())
    { std::cerr << "ImageSurface exposes no Pixels interface -- nothing can be verified\n"; return 1; }

    ETCS::Entity* tool  = ETCS::spawn_entity("PaintProvider", "PaintTool",     env, loader);
    ETCS::Entity* layer = ETCS::spawn_entity("PaintProvider", "PaintLayer",    env, loader);
    ETCS::Entity* doc   = ETCS::spawn_entity("PaintProvider", "PaintDocument", env, loader);
    ETCS::Entity* psurf = ETCS::spawn_entity("PaintProvider", "PaintSurface",  env, loader);
    ETCS::Entity* pin   = ETCS::spawn_entity("PaintProvider", "PaintInput",    env, loader);
    if (!tool || !layer || !doc || !psurf || !pin)
    { std::cerr << "cannot spawn the PaintProvider tags\n"; return 1; }

    layer->call("PaintLayer.Create", SIZE.c_str(), ctx);
    doc->call("PaintDocument.Create", (SIZE + " test").c_str(), ctx);
    doc->call("PaintDocument.AddLayer", rid_arg(layer).c_str(), ctx);
    doc->call("PaintDocument.SetActiveLayer", rid_arg(layer).c_str(), ctx);
    psurf->call("PaintSurface.Create", rid_arg(canvas).c_str(), ctx);
    psurf->call("PaintSurface.AttachDocument", rid_arg(doc).c_str(), ctx);
    pin->call("PaintInput.Create",
              (rid_arg(doc) + " " + rid_arg(tool) + " " + rid_arg(psurf)).c_str(), ctx);

    // ── 1 ────────────────────────────────────────────────────────────────
    std::cout << "\n== 1. the composed reference: does PaintProvider reach another module's surface ==\n";
    canvas->call("ImageSurface.Clear", "1.0 1.0 1.0 1.0", ctx);
    check_pixel(px, W / 2, H / 2, { 255, 255, 255, 255 }, 0, "canvas starts white");

    doc->call("PaintDocument.ClearLayer", (rid_arg(layer) + " 0.0 0.0 1.0 1.0").c_str(), ctx);
    doc->call("PaintDocument.RenderToSurface", (rid_arg(canvas) + " 0 0").c_str(), ctx);
    check_pixel(px, W / 2, H / 2, { 0, 0, 255, 255 }, 2,
                "a composed RID resolved and the composite landed");

    // ── 2: the 2D surface's own arithmetic ───────────────────────────────
    std::cout << "\n== 2. RenderProvider 2D: clear replaces, opaque writes, alpha composites ==\n";
    canvas->call("ImageSurface.Clear", "0.2 0.4 0.6 1.0", ctx);
    check_pixel(px, 10, 10, { 51, 102, 153, 255 }, 1, "Clear REPLACES with the exact colour");

    // Opaque source-over takes the fast path and must be byte-exact.
    canvas->call("ImageSurface.DrawRect", "20 20 40 30 1.0 0.0 0.0 1.0", ctx);
    check_pixel(px, 30, 30, { 255, 0, 0, 255 }, 0, "an opaque DrawRect is the source exactly");
    check_pixel(px, 10, 10, { 51, 102, 153, 255 }, 1, "and does not touch outside its rect");

    // Half-alpha red over the cleared blue: 0.5*255 + 0.5*51 = 153, and
    // 0.5*0 + 0.5*102 = 51, 0.5*0 + 0.5*153 = 76. Derived, not observed.
    canvas->call("ImageSurface.DrawRect", "70 20 40 30 1.0 0.0 0.0 0.5", ctx);
    check_pixel(px, 80, 30, { 153, 51, 76, 255 }, 3, "a half-alpha DrawRect composites source-over");

    // Clipping: a rect that starts off-canvas must land partially, not wrap.
    canvas->call("ImageSurface.Clear", "0.0 0.0 0.0 1.0", ctx);
    canvas->call("ImageSurface.DrawRect", "-10 -10 30 30 0.0 1.0 0.0 1.0", ctx);
    check_pixel(px, 5, 5, { 0, 255, 0, 255 }, 0, "a rect straddling the origin clips, not wraps");
    check_pixel(px, 25, 25, { 0, 0, 0, 255 }, 0, "and stops where it should");
    check_pixel(px, W - 1, H - 1, { 0, 0, 0, 255 }, 0,
                "with nothing wrapped round to the far corner");

    // ── 3: the layer raster ──────────────────────────────────────────────
    std::cout << "\n== 3. PaintLayer raster: bounds, clear, line ==\n";
    layer->call("PaintLayer.Clear", "0.0 0.0 0.0 0.0", ctx);
    // Both guard directions separately: one of them has been wrong before in
    // code shaped like this, and a wrap writes somewhere it was never asked to.
    layer->call("PaintLayer.DrawPixel", "-5 -5 1.0 0.0 0.0 1.0", ctx);
    layer->call("PaintLayer.DrawPixel", "9999 9999 1.0 0.0 0.0 1.0", ctx);
    check(true, "DrawPixel outside the layer is a no-op, both directions");

    canvas->call("ImageSurface.Clear", "1.0 1.0 1.0 1.0", ctx);
    layer->call("PaintLayer.DrawLine", "0 48 127 48 0.0 1.0 0.0 1.0", ctx);
    doc->call("PaintDocument.RenderToSurface", (rid_arg(canvas) + " 0 0").c_str(), ctx);
    // BlitTo subsamples on a 4px grid, so the line at y=48 is carried by the
    // stamp whose top row is y=48. Sampling inside that stamp tests the line;
    // sampling at y=50 would be testing the subsample rate.
    check_pixel(px, 64, 49, { 0, 255, 0, 255 }, 8, "DrawLine composited through the document");
    check_pixel(px, 64, 10, { 255, 255, 255, 255 }, 2, "and left the rest of the canvas alone");

    // ── 4: visibility gating ─────────────────────────────────────────────
    std::cout << "\n== 4. the document composite is gated by the layer ==\n";
    canvas->call("ImageSurface.Clear", "1.0 1.0 1.0 1.0", ctx);
    layer->call("PaintLayer.Clear", "1.0 0.0 0.0 1.0", ctx);
    doc->call("PaintDocument.RenderToSurface", (rid_arg(canvas) + " 0 0").c_str(), ctx);
    check_pixel(px, 64, 48, { 255, 0, 0, 255 }, 8, "a visible opaque layer covers the canvas");

    // ── 5: the stroke state machine, driven by real input events ─────────
    std::cout << "\n== 5. PaintInput: the stroke state machine ==\n";
    {
        // Driven through the exported Pointer/Press/Release verbs, which build
        // an InputEvent and hand it to the same HandleEvent the stream body
        // calls. A loader reaches a module only through its published surface,
        // which is the right constraint -- it is also why those verbs exist.
        tool->call("PaintTool.SetRadius", "4", ctx);
        tool->call("PaintTool.SetColor", "0.0 0.0 1.0 1.0", ctx);
        layer->call("PaintLayer.Clear", "0.0 0.0 0.0 0.0", ctx);
        canvas->call("ImageSurface.Clear", "1.0 1.0 1.0 1.0", ctx);

        // A press before any motion must NOT begin a stroke -- the brush would
        // land at the origin instead of under the pointer. Observed through the
        // canvas: if it began, the origin would be painted.
        pin->call("PaintInput.Press", "", ctx);
        pin->call("PaintInput.Pointer", "20 40", ctx);   // would paint if active
        pin->call("PaintInput.Release", "", ctx);
        doc->call("PaintDocument.RenderToSurface", (rid_arg(canvas) + " 0 0").c_str(), ctx);
        check_pixel(px, 2, 2, { 255, 255, 255, 255 }, 2,
                    "a press before the pointer is seen paints nothing at the origin");

        // Now a real stroke: a position first, then press, then two big jumps
        // that interpolation has to fill in or the line is three dots.
        layer->call("PaintLayer.Clear", "0.0 0.0 0.0 0.0", ctx);
        canvas->call("ImageSurface.Clear", "1.0 1.0 1.0 1.0", ctx);
        pin->call("PaintInput.Pointer", "20 40", ctx);
        pin->call("PaintInput.Press", "", ctx);
        pin->call("PaintInput.Pointer", "60 40", ctx);
        pin->call("PaintInput.Pointer", "100 40", ctx);
        pin->call("PaintInput.Release", "", ctx);
        doc->call("PaintDocument.RenderToSurface", (rid_arg(canvas) + " 0 0").c_str(), ctx);

        check_pixel(px, 20, 41, { 0, 0, 255, 255 }, 8, "the stroke starts where the press was");
        // BETWEEN the samples, not on them: this is the interpolation.
        check_pixel(px, 40, 41, { 0, 0, 255, 255 }, 8,
                    "the gap BETWEEN two samples is filled (stroke interpolation)");
        check_pixel(px, 80, 41, { 0, 0, 255, 255 }, 8, "and so is the second gap");
        check_pixel(px, 124, 41, { 255, 255, 255, 255 }, 2,
                    "while past the last sample stays clean");

        // After release, motion must not paint.
        pin->call("PaintInput.Pointer", "20 80", ctx);
        doc->call("PaintDocument.RenderToSurface", (rid_arg(canvas) + " 0 0").c_str(), ctx);
        check_pixel(px, 20, 81, { 255, 255, 255, 255 }, 2, "motion after release does not paint");
    }

    // ── 5b ───────────────────────────────────────────────────────────────
    std::cout << "\n== 5b. the composed reference, as a set ==\n";
    {
        // collect_in_family is the primitive resolve_in_family is built on: a
        // family spans providers and RIDs are unique only per provider-type, so
        // "the Surface with RID n" is a question that can have more than one
        // answer. Here it has exactly one, which is the case worth pinning --
        // if a second provider ever registers a colliding RID under Surface,
        // this is what changes.
        std::vector<Surface_*> hits;
        const size_t n = ETCS::collect_in_family<Surface_>("Surface", canvas->getRID(), hits);
        check(n == 1, "collect_in_family finds the canvas exactly once ("
              + std::to_string(n) + ")");

        hits.clear();
        const size_t q = ETCS::collect_in_family<Surface_>("RenderProvider:ImageSurface",
                                                            canvas->getRID(), hits);
        check(q == 1, "and the qualified Provider:Type form finds it in one lookup");

        hits.clear();
        check(ETCS::collect_in_family<Surface_>("Surface", 999999999u, hits) == 0,
              "a RID nothing holds returns an empty set, not a null answer");
    }

    // ── 5c ───────────────────────────────────────────────────────────────
    std::cout << "\n== 5c. Orderable::Search across the module boundary ==\n";
    {
        // THE POINT OF THIS SECTION is that this file cannot name
        // PolygonDrawable2D -- the type lives inside RenderProvider.so and no
        // header here declares it. It holds RIDs and Entity*, nothing more.
        // The RID-exemplar form is what makes the question askable anyway: the
        // list resolves the exemplar itself, so the relation stays the type's
        // while the asking does not.
        ETCS::Entity* a = ETCS::spawn_entity("RenderProvider", "PolygonDrawable2D", env, loader);
        ETCS::Entity* b = ETCS::spawn_entity("RenderProvider", "PolygonDrawable2D", env, loader);
        ETCS::Entity* c = ETCS::spawn_entity("RenderProvider", "PolygonDrawable2D", env, loader);
        if (!a || !b || !c) { check(false, "could not spawn three PolygonDrawable2D"); }
        else
        {
            a->call("PolygonDrawable2D.Create", "", ctx);
            b->call("PolygonDrawable2D.Create", "", ctx);
            c->call("PolygonDrawable2D.Create", "", ctx);
            // Drawable2D orders by SetOrder, so two at 7 and one at 3 gives an
            // equivalence class of two -- the case that distinguishes a range
            // from a find.
            a->call("PolygonDrawable2D.SetOrder", "7", ctx);
            b->call("PolygonDrawable2D.SetOrder", "3", ctx);
            c->call("PolygonDrawable2D.SetOrder", "7", ctx);

            std::vector<ETCS::RID> hits;
            const size_t n = Orderable_::Search("RenderProvider:PolygonDrawable2D",
                                                 a->getRID(), hits);
            check(n == 2, "Search by RID exemplar found the equivalence class ("
                  + std::to_string(n) + " of 2 at order 7)");

            bool has_a = false, has_c = false, has_b = false;
            for (ETCS::RID r : hits)
            { if (r == a->getRID()) has_a = true;
              if (r == c->getRID()) has_c = true;
              if (r == b->getRID()) has_b = true; }
            check(has_a && has_c, "and it is the two that share the standing");
            check(!has_b, "with the one that does not left out");

            hits.clear();
            check(Orderable_::Search("RenderProvider:PolygonDrawable2D", b->getRID(), hits) == 1,
                  "a standing held alone returns exactly that one");

            hits.clear();
            check(Orderable_::Search("PolygonDrawable2D", a->getRID(), hits) == 0,
                  "a BARE family name is refused -- an order belongs to one type");

            hits.clear();
            check(Orderable_::Search("RenderProvider:Instance", a->getRID(), hits) == 0,
                  "and a type that declares no order reports it rather than matching");

            a->call("PolygonDrawable2D.Delete", "", ctx);
            b->call("PolygonDrawable2D.Delete", "", ctx);
            c->call("PolygonDrawable2D.Delete", "", ctx);
        }
    }

    // ── 6 ────────────────────────────────────────────────────────────────
    std::cout << "\n== 6. teardown is clean and idempotent ==\n";
    pin->call("PaintInput.Delete", "", ctx);
    psurf->call("PaintSurface.Delete", "", ctx);
    doc->call("PaintDocument.Delete", "", ctx);
    layer->call("PaintLayer.Delete", "", ctx);
    tool->call("PaintTool.Delete", "", ctx);
    // Twice, because Delete is reachable from a script and from the arena and a
    // second arrival must find the work already done.
    tool->call("PaintTool.Delete", "", ctx);
    check(true, "Delete on every tag, twice on one, did not fault");
    canvas->call("ImageSurface.Delete", "", ctx);

    std::cout << "\n=== Summary: " << g_pass << " passed, " << g_fail << " failed ===\n";
    return g_fail == 0 ? 0 : 1;
}
