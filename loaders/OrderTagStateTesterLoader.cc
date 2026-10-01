// OrderTagStateTesterLoader.cc
//
// AN ORDERABLE'S KEY IS ITS TAG STATE, AND A TAG CHANGE RE-ORDERS IT.
//
// Two changes, held to account together because neither shows without the
// other:
//
//   1. Every drawable's operator< (and Drawable_::Rank, the cross-type merge)
//      opens with "hidden loses every comparison".
//   2. Any change to an Orderable's own tag state marks its parent's list stale
//      (Entity::markStateChange), so the lazily sorted RIDList notices a key
//      that moved while membership stayed put.
//
// The flags below go through the plain flag funnel -- what a script's
// `.unflag(hidden)` does -- never SetOrder, which already called Reorder().
// Before either change, every ordered read here answers the unhidden order.
//
//   ./Run_OrderTagStateTesterLoader
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

static std::vector<ETCS::RID> ordered(ETCS::Entity* parent)
{
    std::vector<std::pair<ETCS::Buffer, ETCS::RID>> kids;
    parent->getOrderedTypedChildren(kids);
    std::vector<ETCS::RID> out;
    for (const auto& k : kids) out.push_back(k.second);
    return out;
}

static std::vector<ETCS::RID> merged(ETCS::Entity* parent)
{
    auto* d = static_cast<Drawable_*>(parent->getInterfacePointer(ETCS::Buffer("Drawable")));
    std::vector<Drawable_*> kids;
    if (d) d->collectDrawableChildren(kids);
    std::vector<ETCS::RID> out;
    for (Drawable_* k : kids) out.push_back(k->getRID());
    return out;
}

static ETCS::Entity* child(const char* tag, ETCS::Entity* parent, int order, const char* size,
                           ETCS::SignalContext ctx)
{
    ETCS::ExecSource src{ "OrderTagStateTesterLoader", 0 };
    ETCS::Entity* e = ETCS::make_typed_child("RenderProvider", tag, parent, src);
    if (!e) return nullptr;
    const std::string t(tag);
    e->call((t + ".Create").c_str(), size, ctx);
    e->call((t + ".SetOrder").c_str(), std::to_string(order).c_str(), ctx);
    return e;
}

int main(int, char**)
{
    WIRE_CONTEXT();

    std::printf("\n-- within one type: the flag moves the key -----------------------\n");

    ETCS::Entity* p = ETCS::spawn_entity("RenderProvider", "CompositeDrawable2D", env, loader);
    check(p != nullptr, "a parent spawns");
    if (!p) return 1;
    p->call("CompositeDrawable2D.Create", "64 64", ctx);
    ETCS::Entity* a = child("CompositeDrawable2D", p, 5, "8 8", ctx);
    ETCS::Entity* b = child("CompositeDrawable2D", p, 1, "8 8", ctx);
    ETCS::Entity* c = child("CompositeDrawable2D", p, 3, "8 8", ctx);
    check(a && b && c, "three children spawn");
    if (!(a && b && c)) return 1;
    const ETCS::RID A = a->getRID(), B = b->getRID(), C = c->getRID();

    // Read once, so the list is sorted and cached before anything moves.
    check(ordered(p) == std::vector<ETCS::RID>{ B, C, A }, "stated order: b(1) c(3) a(5)");

    a->addTag(ETCS::Buffer("hidden"));
    // Before: b c a -- the list was never told, and the relation ignored the flag.
    check(ordered(p) == std::vector<ETCS::RID>{ A, B, C },
          "a raised `hidden`: it loses to both, and the cached order noticed");

    a->removeTag(ETCS::Buffer("hidden"));
    check(ordered(p) == std::vector<ETCS::RID>{ B, C, A }, "a cleared it: back to b c a");

    // A burst between two reads: only the state at the read matters.
    c->addTag(ETCS::Buffer("busy"));
    c->addTag(ETCS::Buffer("hidden"));
    c->removeTag(ETCS::Buffer("busy"));
    b->addTag(ETCS::Buffer("busy"));
    check(ordered(p) == std::vector<ETCS::RID>{ C, B, A },
          "a burst of flags, one read: hidden c first, the rest by order");

    c->call("CompositeDrawable2D.SetHidden", "0", ctx);
    a->call("CompositeDrawable2D.SetHidden", "1", ctx);
    check(ordered(p) == std::vector<ETCS::RID>{ A, B, C }, "through SetHidden too: a now loses");
    a->call("CompositeDrawable2D.SetHidden", "0", ctx);

    std::printf("\n-- across types: Drawable_::Rank ---------------------------------\n");

    ETCS::Entity* q = ETCS::spawn_entity("RenderProvider", "CompositeDrawable2D", env, loader);
    if (!q) return 1;
    q->call("CompositeDrawable2D.Create", "64 64", ctx);
    ETCS::Entity* x = child("CompositeDrawable2D", q, 10, "8 8", ctx);
    ETCS::Entity* t = child("TextLabel", q, 2, "", ctx);
    check(x && t, "a composite and a label under one parent");
    if (!(x && t)) return 1;
    const ETCS::RID X = x->getRID(), T = t->getRID();

    check(merged(q) == std::vector<ETCS::RID>{ T, X }, "merged by Order(): label(2) then composite(10)");
    x->addTag(ETCS::Buffer("hidden"));
    // Before: T X -- the merge compared Order() alone.
    check(merged(q) == std::vector<ETCS::RID>{ X, T }, "the composite hidden: it loses across types too");
    auto* xd = static_cast<Drawable_*>(x->getInterfacePointer(ETCS::Buffer("Drawable")));
    check(xd && xd->Order() == 10, "...and its Order() is still the stated 10: the projection is in the comparison");
    x->removeTag(ETCS::Buffer("hidden"));
    check(merged(q) == std::vector<ETCS::RID>{ T, X }, "shown again, back in order");

    q->call("CompositeDrawable2D.Delete", "", ctx);
    p->call("CompositeDrawable2D.Delete", "", ctx);
    ETCS::PendingUnloadRegistry::getInstance().join_all();

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
