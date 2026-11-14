// HiddenFlagTesterLoader.cc
//
// HIDDEN IS A FLAG, AND THE FLAG IS FOUND AGAIN WHERE IT LANDS.
//
// Two changes held to account here, because they only make sense together:
//
//   1. `hidden` lives in the entity's own tag store, beside `passthrough`,
//      instead of a private bool on DrawableBase. Showing and hiding are
//      meaningful state, so they go through the flag funnel that records
//      state: the hash moves, observers are marked, a script can ask.
//      Hidden() is a cache of the flag, stamped with the node's hash epoch,
//      because it is read for every child on every compose and pick.
//
//   2. A flag change names its target by RID and type, and the ordering
//      thread re-resolves that name where the change lands. A RID
//      resolution is the liveness check -- which is what lets a verb that
//      raises a flag be called without holding the node across the wait.
//
// Each check says what it would have read before the change, so a failure
// here points at which half regressed.
//
//   ./Run_HiddenFlagTesterLoader
//
// No display, no GPU: CompositeDrawable2D and Throbber are pure CPU.

#include "../ETCS.h"

#include <cstdio>
#include <string>

static int g_fail = 0;

static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

static Drawable_* as_drawable(ETCS::Entity* e)
{
    void* p = e ? e->getInterfacePointer(ETCS::Buffer("Drawable")) : nullptr;
    return static_cast<Drawable_*>(p);
}
static Drawable2D_* as_2d(ETCS::Entity* e)
{
    void* p = e ? e->getInterfacePointer(ETCS::Buffer("Drawable2D")) : nullptr;
    return static_cast<Drawable2D_*>(p);
}
static bool flagged(ETCS::Entity* e) { return e->hasTag(ETCS::Buffer("hidden")); }

int main(int, char**)
{
    WIRE_CONTEXT();

    std::printf("\n-- the flag is the state ---------------------------------------\n");

    ETCS::Entity* pane = ETCS::spawn_entity("RenderProvider", "CompositeDrawable2D", env, loader);
    check(pane != nullptr, "RenderProvider:CompositeDrawable2D spawns");
    if (!pane) return 1;
    pane->call("CompositeDrawable2D.Create", "64 64", ctx);
    Drawable_* d = as_drawable(pane);
    check(d != nullptr, "it answers as a Drawable");
    if (!d) return 1;

    check(!d->Hidden() && !flagged(pane), "a new node is shown, and carries no flag");

    const uint64_t hash_shown = pane->getHash();
    pane->call("CompositeDrawable2D.SetHidden", "1", ctx);
    // Before: SetHidden wrote a private bool, so this was false.
    check(flagged(pane), "SetHidden(1) raises the `hidden` flag in the node's own store");
    check(d->Hidden(), "...and Hidden() answers it");
    // Before: the bool changed and the node hash did not.
    check(pane->getHash() != hash_shown, "...and the node hash moved: it is recorded state");

    const uint32_t epoch = pane->hashEpoch();
    pane->call("CompositeDrawable2D.SetHidden", "1", ctx);
    check(pane->hashEpoch() == epoch,
          "hiding what is already hidden is no transition -- the setter asks first");

    // What `.unflag(hidden)` does from a script. The cache must follow the
    // FLAG, not the setter, or a script could clear it and still see nothing.
    pane->removeTag(ETCS::Buffer("hidden"));
    check(!d->Hidden(), "clearing the flag directly shows the node: the cache follows the flag");
    pane->addTag(ETCS::Buffer("hidden"));
    check(d->Hidden(), "raising it directly hides it again");
    pane->call("CompositeDrawable2D.SetHidden", "0", ctx);
    check(!d->Hidden() && !flagged(pane), "SetHidden(0) takes the flag off");

    std::printf("\n-- a hidden node is not hit -------------------------------------\n");

    // One child covering the parent. Picked while shown; while hidden the pick
    // falls through to the parent, which is what an invisible window must do.
    ETCS::ExecSource src{ "HiddenFlagTesterLoader", 0 };
    ETCS::Entity* child = ETCS::make_typed_child("RenderProvider", "CompositeDrawable2D", pane, src);
    check(child != nullptr, "a child spawns under it");
    if (child)
    {
        child->call("CompositeDrawable2D.Create", "64 64", ctx);
        Drawable2D_* p2 = as_2d(pane);
        Drawable2D_* c2 = as_2d(child);
        Pick2D hit = p2->PickAt(Point2D{ 10, 10 });
        check(hit.node == c2, "shown, the child takes the pick");
        child->call("CompositeDrawable2D.SetHidden", "1", ctx);
        hit = p2->PickAt(Point2D{ 10, 10 });
        check(hit.node != c2, "hidden, it does not -- PickAt reads the flag through the cache");
        child->call("CompositeDrawable2D.SetHidden", "0", ctx);
        hit = p2->PickAt(Point2D{ 10, 10 });
        check(hit.node == c2, "shown again, it does");
    }

    std::printf("\n-- a watched throbber derives, it does not mirror ---------------\n");

    // Its visibility follows ANOTHER entity's flag, which is already the record.
    // A second `hidden` flag written to mirror it would be the same fact twice,
    // and would have to be written from inside the compose walk.
    ETCS::Entity* ring = ETCS::spawn_entity("RenderProvider", "Throbber", env, loader);
    check(ring != nullptr, "RenderProvider:Throbber spawns");
    if (ring)
    {
        ring->call("Throbber.Create", "48", ctx);
        ring->call("Throbber.Watch", (std::to_string(pane->getRID()) + ", busy").c_str(), ctx);
        auto* anim = static_cast<Animated_*>(ring->getInterfacePointer(ETCS::Buffer("Animated")));
        Drawable_* rd = as_drawable(ring);
        check(anim && rd, "it is Animated and Drawable");
        if (anim && rd)
        {
            anim->Animating();                              // the frame edge's question
            check(rd->Hidden(), "watched flag down: the ring is hidden");
            check(!flagged(ring), "...without a `hidden` flag of its own");

            pane->addTag(ETCS::Buffer("busy"));
            anim->Animating();
            check(!rd->Hidden(), "watched flag up: the ring shows");
            check(!flagged(ring), "...still without writing one");

            pane->removeTag(ETCS::Buffer("busy"));
            anim->Animating();
            check(rd->Hidden(), "and down again, hidden");
        }
    }

    std::printf("\n-- a flag change finds its target again --------------------------\n");

    // The handler's resolution, asked directly with the event it would be given.
    ETCS::DLInEvent evt{};
    evt.kind             = ETCS::DLInEvent::Kind::TagModify;
    evt.conjugate_key    = ETCS::Buffer("probe");
    evt.tagmodify_type   = pane->myConjugateKey();
    evt.tagmodify_rid    = pane->getRID();
    evt.tagmodify_target = pane;
    check(etcs_tagmodify_target(evt) == pane, "a live, listed entity resolves to itself");

    // The address is still aboard. For a listed type it must NOT be believed:
    // the list is the authority, and a RID it does not hold is a dead entity.
    evt.tagmodify_rid = pane->getRID() ^ 0x5a5a5a5a5a5aULL;
    check(etcs_tagmodify_target(evt) == nullptr,
          "a listed type whose list lacks the RID is refused, whatever the address says");

    // Unlisted: nothing in the runtime could answer, so the emitter's word stands.
    evt.tagmodify_type = ETCS::Buffer("NoTypeByThisName");
    check(etcs_tagmodify_target(evt) == pane,
          "an unlisted type falls back to the address -- there is no list to ask");

    // The real case: retired while a change could still have been queued.
    ETCS::Entity* doomed = ETCS::spawn_entity("RenderProvider", "CompositeDrawable2D", env, loader);
    if (doomed)
    {
        doomed->call("CompositeDrawable2D.Create", "8 8", ctx);
        ETCS::DLInEvent late{};
        late.kind             = ETCS::DLInEvent::Kind::TagModify;
        late.conjugate_key    = ETCS::Buffer("hidden");
        late.tagmodify_type   = doomed->myConjugateKey();
        late.tagmodify_rid    = doomed->getRID();
        late.tagmodify_target = doomed;                // an address that is about to go bad
        doomed->call("CompositeDrawable2D.Delete", "", ctx);
        ETCS::PendingUnloadRegistry::getInstance().join_all();
        check(etcs_tagmodify_target(late) == nullptr,
              "an entity deleted before its change lands is refused, not written into");
    }

    if (ring) ring->call("Throbber.Delete", "", ctx);
    pane->call("CompositeDrawable2D.Delete", "", ctx);
    ETCS::PendingUnloadRegistry::getInstance().join_all();

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
