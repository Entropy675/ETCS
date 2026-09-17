// FamilyMirrorTesterLoader.cc
//
// ONE FAMILY, TWO PROVIDERS -- the case the loader's RIDList mirror exists for,
// and the one nothing exercised until it broke.
//
// Deletable is claimed by every provider in the tree; Observable and Resizable
// by several. So the loader's view of a family is a SET of lists, one per image
// that published the name, and the only interesting question about it is whether
// a second publisher can make the first one disappear.
//
// IT COULD. While the mirror was a single handle per bare name, absorbing a
// module was `ridMap[name] = handle` -- an overwrite -- so the last module to
// load owned the row and every earlier provider's entities stopped resolving by
// family. Nothing caught it because every existing test loads one provider at a
// time, and a module resolving its OWN entity answers out of its own map without
// ever consulting the loader. It takes two providers AND a loader-side resolve,
// which is exactly what this file is.
//
// The teardown half is the same bug with the sign flipped: rows were dropped by
// NAME, so unloading one module erased whichever provider happened to hold the
// shared row. Checked here too, since a mirror that leaks a stale handle into
// unmapped memory fails later and somewhere else.
//
//   ./Run_FamilyMirrorTesterLoader
//
// No display, no GPU, no network: ImageSurface and Layout are both pure CPU.

#include "../ETCS.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

static int g_fail = 0;

static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

int main(int, char**)
{
    WIRE_CONTEXT();

    // -- 1. one provider, one entity, resolvable by family --------------------
    ETCS::Entity* a = ETCS::spawn_entity("RenderProvider", "ImageSurface", env, loader);
    check(a != nullptr, "RenderProvider:ImageSurface spawns");
    if (!a) return 1;
    a->call("ImageSurface.Create", "32 32", ctx);

    check(ETCS::resolve_in_family<Deletable_>("Deletable", a->getRID()) != nullptr,
          "it resolves as a Deletable with one provider loaded");

    // -- 2. a SECOND provider claiming the same family ------------------------
    //
    // The whole test. Nothing about A changed; if A stops resolving here, the
    // mirror merged two providers into one row.
    ETCS::Entity* b = ETCS::spawn_entity("LayoutProvider", "Layout", env, loader);
    check(b != nullptr, "LayoutProvider:Layout spawns");
    if (!b) return 1;
    b->call("Layout.Create", "32 32", ctx);

    check(ETCS::resolve_in_family<Deletable_>("Deletable", a->getRID()) != nullptr,
          "A still resolves as a Deletable after a second provider loaded");
    check(ETCS::resolve_in_family<Deletable_>("Deletable", b->getRID()) != nullptr,
          "B resolves as a Deletable too");

    // Both, from one bare-family ask. collect_in_family is the honest shape --
    // resolve_in_family can only answer with one -- and it is also where a
    // single-address-space build used to double-count, reading the same list
    // once directly and once through the mirror.
    {
        std::vector<Deletable_*> found;
        ETCS::collect_in_family<Deletable_>("Deletable", a->getRID(), found);
        check(found.size() == 1, "collect_in_family returns A exactly once, not twice");
    }

    // -- 3. the mirror holds one row PER PROVIDER -----------------------------
    ETCS::EventNode* ldr = ETCS::etcs_loader_event_node();
    check(ldr != nullptr, "the loader EventNode is reachable");
    if (ldr)
    {
        auto it = ldr->ridMirror.find(ETCS::Buffer("Deletable"));
        const size_t rows = (it == ldr->ridMirror.end()) ? 0 : it->second.size();
        check(rows >= 2, "the Deletable mirror bucket has a row for each provider");
        std::printf("        (Deletable rows: %zu)\n", rows);
    }

    // -- 4. naming the provider narrows it ------------------------------------
    //
    // A qualified ask is a caller SAYING which provider it meant, which is the
    // disambiguation resolve_in_family's ambiguity report asks for. It must
    // answer for that provider and refuse for the other.
    check(ETCS::resolve_in_family<Deletable_>("RenderProvider:Deletable", a->getRID()) != nullptr,
          "RenderProvider:Deletable resolves A");
    check(ETCS::resolve_in_family<Deletable_>("LayoutProvider:Deletable", a->getRID()) == nullptr,
          "LayoutProvider:Deletable does NOT resolve A");

    // -- 5. per-type rows, by bare name and by qualified name -----------------
    check(ETCS::etcs_ridmap_named(ldr, ETCS::Buffer("RenderProvider:ImageSurface")) != nullptr,
          "a per-type list is reachable by qualified name");
    check(ETCS::etcs_ridmap_named(ldr, ETCS::Buffer("ImageSurface")) != nullptr,
          "...and by bare name, since one provider publishes it");
    check(ETCS::etcs_resolve_by_key(ETCS::Buffer("RenderProvider:ImageSurface"), a->getRID()) != nullptr,
          "etcs_resolve_by_key finds A by its conjugate key");
    check(ETCS::etcs_resolve_by_key(ETCS::Buffer("LayoutProvider:Layout"), b->getRID()) != nullptr,
          "...and B by its own");

    // -- 5b. A BARE RID, NO TYPE, NO FAMILY -----------------------------------
    //
    // The walk four modules hand-wrote over getLoader().ridMap, and the one that
    // broke silently when the absorbed lists moved into the mirror: a registered
    // subscriber stopped resolving at dispatch, self-healed out of its own list,
    // and the server dropped every connection it accepted. Checked for BOTH
    // providers, because a single-provider check passes against a map that only
    // ever holds one.
    check(ETCS::etcs_resolve_rid_anywhere(ldr, a->getRID()) == a,
          "etcs_resolve_rid_anywhere finds A from a bare RID");
    check(ETCS::etcs_resolve_rid_anywhere(ldr, b->getRID()) == b,
          "...and B, in a different module's image");
    check(ETCS::etcs_resolve_rid_anywhere(ldr, 0) == nullptr,
          "...and answers null for RID 0 rather than the first row");

    // -- 5c. A QUALIFIED ASK WHEN NOTHING MIRRORED THE NAME -------------------
    //
    // THE COLLAPSED BUILD, which is what emscripten (and the kernel path later)
    // actually is: one address space, the module's EventNode IS the loader's, so
    // registerLoader skips the absorb and every module's list sits in ridMap with
    // ridMirror empty. A qualified lookup that insists on a mirror row then
    // refuses with the list in the very map it declined to read -- and that broke
    // EVERY qualified lookup in the browser: the shell re-resolving the entity it
    // was standing on, a global name's liveness check, a bound entity by
    // conjugate key.
    //
    // Cannot be reproduced natively by loading modules -- the mirror is never
    // empty here -- so the RULE is tested directly: a name this image publishes
    // and nothing mirrors must answer a qualified ask naming any module, because
    // with one image this image IS that module.
    if (ldr)
    {
        const ETCS::Buffer probe_name("ZZCollapsedProbe");
        ETCS::RIDList<ETCS::Entity*> probe_list;
        ldr->ridMap[probe_name] = probe_list.handle("ZZCollapsedProbe");

        check(ETCS::etcs_ridmap_named(ldr, probe_name) != nullptr,
              "an unmirrored name answers a bare ask");
        check(ETCS::etcs_ridmap_named(ldr, ETCS::Buffer("AnyProvider:ZZCollapsedProbe")) != nullptr,
              "...and answers a QUALIFIED ask naming a foreign module (collapsed build)");

        // And the opposite must still hold: a name that IS mirrored refuses a
        // module that did not publish it, rather than falling through.
        check(ETCS::etcs_ridmap_named(ldr, ETCS::Buffer("NoSuchProvider:Deletable")) == nullptr,
              "a mirrored name still refuses a module that never published it");

        ldr->ridMap.erase(probe_name);
    }

    // -- 6. a bare FAMILY name with no RID has no single answer ---------------
    //
    // Refused rather than guessed: several providers publish Deletable, so
    // "the Deletable list" is not a thing. The same refusal resolve_in_family
    // makes on a genuine ambiguity, for the same reason -- a wrong answer here
    // is indistinguishable from a right one.
    check(ETCS::etcs_ridmap_named(ldr, ETCS::Buffer("Deletable")) == nullptr,
          "a multi-provider family refuses a no-RID lookup");

    // -- 7. fan-in removes it from the family list ----------------------------
    const ETCS::RID a_rid = a->getRID();
    a->call("ImageSurface.Delete", "", ctx);
    ETCS::PendingUnloadRegistry::getInstance().join_all();
    check(ETCS::resolve_in_family<Deletable_>("Deletable", a_rid) == nullptr,
          "a deleted entity leaves the family list");
    check(ETCS::resolve_in_family<Deletable_>("Deletable", b->getRID()) != nullptr,
          "...and B, in another provider's list, is untouched by it");

    b->call("Layout.Delete", "", ctx);
    ETCS::PendingUnloadRegistry::getInstance().join_all();

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
