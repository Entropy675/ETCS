// PaintRouterKeysTesterLoader.cc
//
// A CLOSED WINDOW DOES NOT ANSWER THE KEYBOARD.
//
// PaintRouter::RouteKey offers a key to each pane's input, highest order first,
// and the first one that consumes it wins. A closed window keeps its order, so
// unless the walk skips hidden panes, a closed popup at order 30 is asked
// before the canvas at 0. Its input has no pages bound and consumes ctrl+PageDown
// with "no pages bound", which is how the page chords went dead on the page.
//
// Before the fix, section 1 counts no page: the chord never reached the input
// that holds them. Section 2 is the other half: a SHOWN popup must still be
// asked first, so skipping hidden panes must not become "skip popups".
//
//   ./Run_PaintRouterKeysTesterLoader
//
// No display, no GPU. The page count is read from the database the pages write.

#include "../ETCS.h"

#include <cstdio>
#include <string>
#include <unistd.h>

static int g_fail = 0;

static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static std::string rid_arg(ETCS::Entity* e) { return std::to_string(e->getRID()); }

static int64_t count_pages(ETCS::Entity* db)
{
    auto* d = static_cast<Database_*>(db->getInterfacePointer(ETCS::Buffer("Database")));
    if (!d) return -1;
    void* st = d->Prepare("SELECT COUNT(*) FROM pages");
    if (!st) return -1;
    int64_t n = -1;
    DatabaseValue v;
    if (d->Step(st) == 1 && d->Column(st, 0, v) && v.kind == DatabaseValue::Integer) n = v.i;
    d->Finalize(st);
    return n;
}

int main(int, char**)
{
    WIRE_CONTEXT();

    constexpr const char* SIZE = "64 48";
    constexpr int KEY_LEFT_CONTROL = 341, KEY_PAGE_DOWN = 267;   // GLFW codes

    ETCS::Entity* image = ETCS::spawn_entity("RenderProvider", "ImageSurface",  env, loader);
    ETCS::Entity* tool  = ETCS::spawn_entity("PaintProvider",  "PaintTool",     env, loader);
    ETCS::Entity* doc   = ETCS::spawn_entity("PaintProvider",  "PaintDocument", env, loader);
    ETCS::Entity* psurf = ETCS::spawn_entity("PaintProvider",  "PaintSurface",  env, loader);
    ETCS::Entity* db    = ETCS::spawn_entity("DatabaseProvider", "LocalDatabase", env, loader);
    ETCS::Entity* pages = ETCS::spawn_entity("PaintProvider",  "PaintPages",    env, loader);
    ETCS::Entity* on_canvas = ETCS::spawn_entity("PaintProvider", "PaintInput", env, loader);
    ETCS::Entity* on_popup  = ETCS::spawn_entity("PaintProvider", "PaintInput", env, loader);
    ETCS::Entity* sheet = ETCS::spawn_entity("RenderProvider", "CompositeDrawable2D", env, loader);
    ETCS::Entity* popup = ETCS::spawn_entity("RenderProvider", "CompositeDrawable2D", env, loader);
    ETCS::Entity* router = ETCS::spawn_entity("PaintProvider", "PaintRouter",   env, loader);
    check(image && tool && doc && psurf && db && pages && on_canvas && on_popup
          && sheet && popup && router, "everything spawns");
    if (!(image && tool && doc && psurf && db && pages && on_canvas && on_popup
          && sheet && popup && router)) return 1;

    image->call("ImageSurface.Create", SIZE, ctx);
    doc->call("PaintDocument.Create", (std::string(SIZE) + " keys").c_str(), ctx);
    ETCS::Entity* layer = ETCS::make_typed_child("PaintProvider", "PaintLayer", doc, loader);
    if (!layer) { std::printf("cannot spawn a layer under the document\n"); return 1; }
    layer->call("PaintLayer.Create", SIZE, ctx);
    doc->call("PaintDocument.SetActiveLayer", rid_arg(layer).c_str(), ctx);
    psurf->call("PaintSurface.Create", rid_arg(image).c_str(), ctx);
    psurf->call("PaintSurface.AttachDocument", rid_arg(doc).c_str(), ctx);

    const std::string path = "/tmp/etcs_router_keys_" + std::to_string(getpid()) + ".db";
    ::unlink(path.c_str());
    db->call("LocalDatabase.Connect", path.c_str(), ctx);
    pages->call("PaintPages.Create", (rid_arg(doc) + " " + rid_arg(db)).c_str(), ctx);
    check(count_pages(db) == 0, "the store starts empty");

    const std::string trio = rid_arg(doc) + " " + rid_arg(tool) + " " + rid_arg(psurf);
    on_canvas->call("PaintInput.Create", trio.c_str(), ctx);
    on_canvas->call("PaintInput.BindPages", rid_arg(pages).c_str(), ctx);
    on_popup->call("PaintInput.Create", trio.c_str(), ctx);           // no pages: it swallows the chord

    sheet->call("CompositeDrawable2D.Create", SIZE, ctx);
    sheet->call("CompositeDrawable2D.SetOrder", "0", ctx);
    popup->call("CompositeDrawable2D.Create", "32 32", ctx);
    popup->call("CompositeDrawable2D.SetOrder", "30", ctx);
    popup->call("CompositeDrawable2D.SetHidden", "1", ctx);

    router->call("PaintRouter.Create", "1", ctx);
    router->call("PaintRouter.AddPane", (rid_arg(sheet) + " " + rid_arg(on_canvas)).c_str(), ctx);
    router->call("PaintRouter.AddPane", (rid_arg(popup) + " " + rid_arg(on_popup)).c_str(), ctx);

    auto key = [&](int k) { router->call("PaintRouter.Key", std::to_string(k).c_str(), ctx); };
    key(KEY_LEFT_CONTROL);             // held from here on: nothing below releases it

    std::printf("\n-- a closed window above the canvas is not asked ----------------\n");
    key(KEY_PAGE_DOWN);
    // Before: 0 -- the hidden popup ranked first and swallowed the chord.
    // An unchanged first page is not kept; the switch opens one and saves it.
    check(count_pages(db) == 1, "ctrl+PageDown reaches the canvas: a page is opened and stored");

    std::printf("\n-- an open window above the canvas still is ---------------------\n");
    popup->call("CompositeDrawable2D.SetHidden", "0", ctx);
    const int64_t before = count_pages(db);
    key(KEY_PAGE_DOWN);
    check(count_pages(db) == before, "shown, the popup is asked first and keeps the key");
    popup->call("CompositeDrawable2D.SetHidden", "1", ctx);
    key(KEY_PAGE_DOWN);
    check(count_pages(db) == before + 1, "closed again, the key goes past it to the canvas");

    router->call("PaintRouter.Delete", "", ctx);
    pages->call("PaintPages.Destroy", "", ctx);
    on_canvas->call("PaintInput.Delete", "", ctx);
    on_popup->call("PaintInput.Delete", "", ctx);
    popup->call("CompositeDrawable2D.Delete", "", ctx);
    sheet->call("CompositeDrawable2D.Delete", "", ctx);
    doc->call("PaintDocument.Delete", "", ctx);
    tool->call("PaintTool.Delete", "", ctx);
    image->call("ImageSurface.Delete", "", ctx);
    db->call("LocalDatabase.Delete", "", ctx);
    ETCS::PendingUnloadRegistry::getInstance().join_all();
    ::unlink(path.c_str());

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
