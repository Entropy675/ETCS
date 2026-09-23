// UnreachedActionTesterLoader.cc
//
// A CALL THAT WAS MADE AND NEVER ARRIVED -- the one failure this runtime cannot
// catch at build time, cannot catch at load, and used to describe in six words.
//
// A work function listed in a tag block with no DEFINE_WORK_FUNC is a compile
// error: ETCS_MODULE_EXPORT_WORK takes the address of the trampoline. The other
// direction is not. A DEFINE_WORK_FUNC that is never listed compiles, links, and
// leaves an external-linkage symbol nothing references -- so the call reaches
// ModuleBundle::operator(), finds no action, and returns false. Every layer
// below that point has already lost the distinction: a work function that ran
// and wrote nothing leaves exactly the buffer a missing one does, and
// ETCS_CAUSAL_SCOPE lives INSIDE the trampoline, so nothing that records from
// there can record a call that never got there.
//
// So etcs_report_unreached_action is the only site holding both halves -- asked,
// and did not arrive -- and this file is what holds it to that. The assertions
// are about the REPORT, not about the return value, because the return value was
// always right and was never the thing anybody read.
//
//   ./Run_UnreachedActionTesterLoader
//
// No display, no GPU, no network: LayoutProvider is pure CPU, and the synthetic
// half builds ModuleBundles directly and never dispatches through one.

#include "../ETCS.h"

#include <cstdio>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>

static int g_fail = 0;

static void check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

/*
 * THE LOG IS THE SUBJECT, so it is captured rather than watched.
 *
 * ETCS::log_sink is the sanctioned hook (Log.h) and LogSinkGuard restores the
 * previous one on the way out, which matters because it is thread_local and
 * these bodies dispatch. Capturing cout's rdbuf instead would also catch every
 * other thread's logging and turn an assertion about this message into an
 * assertion about scheduling.
 */
static std::string captured(const std::function<void()>& body)
{
    std::ostringstream sink;
    ETCS::LogSinkGuard guard(&sink);
    body();
    return sink.str();
}

static bool has(const std::string& hay, const char* needle)
{
    return hay.find(needle) != std::string::npos;
}

// A ModuleBundle with a catalog this file chose, dispatching to nothing. Only
// ever asked for actions it does NOT have: a hit would call through a null
// workFunc, which is a different test's business (WorkBundle's own guards).
static void put(ETCS::ModuleBundle& mb, const char* name, bool stream)
{
    mb.actions.insert(&ETCS::MemoryArena::getInstance(), ETCS::Buffer(name),
                      ETCS::WorkBundle(mb.tag, ETCS::Buffer(name), nullptr, 0, stream));
}

int main(int, char**)
{
    WIRE_CONTEXT();

    std::printf("\n-- the real dispatch path ------------------------------------\n");

    // -- 1. an unknown action on a live entity --------------------------------
    //
    // Entity::call -> hasTag("Layout") passes -> the bundle is found -> the
    // ACTION is not. This is the exact shape of a tag-block omission from the
    // caller's side, and the only difference between it and a typo is which of
    // the two names is wrong.
    ETCS::Entity* e = ETCS::spawn_entity("LayoutProvider", "Layout", env, loader);
    check(e != nullptr, "LayoutProvider:Layout spawns");
    if (!e) return 1;

    const std::string missed = captured([&] { e->call("Layout.NoSuchAction", "", ctx); });

    check(has(missed, "does not provide requested action"), "an unknown action is reported");
    check(has(missed, "NoSuchAction"),      "...naming the action that was asked for");
    check(has(missed, "Layout"),            "...naming the tag that was asked");
    check(has(missed, "provides:"),         "...and what that tag does provide");

    // The list is READ FROM THE CATALOG, not echoed from the request. Three of
    // Layout's real actions, none of which appear anywhere in the call above --
    // so a report that prints them cannot be repeating what it was handed.
    check(has(missed, "AddBox") && has(missed, "FollowResize") && has(missed, "SetDirection"),
          "...and the list is the real catalog, not an echo of the request");

    // The sentence that separates a typo from an omission. Without it the list
    // above is a puzzle; with it, an action you know you wrote is an answer.
    check(has(missed, "ETCS_TAG_BLOCK"),
          "...and it names the tag block as the place a defined action goes missing");

    check(!has(missed, "stream action"), "a work action is not described as a stream one");

    // -- 2. no false positive -------------------------------------------------
    //
    // The report must be silent on the ordinary path, or it is noise and the
    // next person filters it out -- which is how the refusal that prompted this
    // patch went unread once per row per refresh.
    const std::string fine = captured([&] { e->call("Layout.Create", "64 64", ctx); });
    check(!has(fine, "does not provide"), "an action that exists is reported not at all");

    std::printf("\n-- the branches a live module cannot reach -------------------\n");

    // -- 3. the stream overload ----------------------------------------------
    //
    // A second entry point into the same else. It had its own copy of the
    // message before this change, which is two places to keep in step for one
    // fact; it is one function now, and this is what says so.
    ETCS::ModuleBundle mb;
    mb.tag = ETCS::Buffer("Synthetic");
    put(mb, "Alpha", false);
    put(mb, "Flow",  true);
    put(mb, "Zeta",  false);

    ETCS::MBuffer mdata;
    bool stream_ret = true;
    const std::string streamed = captured([&] {
        stream_ret = mb(ETCS::RID(1), ETCS::Buffer("Synthetic"), ETCS::Buffer("Absent"),
                        mdata, ctx);
    });
    check(has(streamed, "does not provide requested stream action"),
          "the stream overload says stream");
    check(has(streamed, "Absent") && has(streamed, "Alpha"),
          "...and reports the same way: what was asked, and what there is");
    check(stream_ret == false, "...and still returns false, as the contract says");

    // -- 4. a stream entry is marked in the list ------------------------------
    //
    // Worth a line because the fix for "action not found" is different when the
    // action exists but is a stream: it is reached through MirrorBuffer, not
    // through Entity::call, and a list that did not distinguish them would send
    // you looking for a spelling mistake that is not there.
    check(has(streamed, "Flow (stream)"),
          "a stream in the provides list is marked as one");

    // -- 5. an empty catalog is its own fault ---------------------------------
    //
    // Not "action missing" -- NOTHING was catalogued, which means no tag block
    // reached discoverActions for this tag. A different fault with a different
    // fix, and a provides list reading "" would have looked like the first one.
    ETCS::ModuleBundle empty;
    empty.tag = ETCS::Buffer("Hollow");
    ETCS::Buffer edata;
    bool empty_ret = true;
    const std::string hollow = captured([&] {
        empty_ret = empty(ETCS::RID(1), ETCS::Buffer("Hollow"), ETCS::Buffer("Anything"),
                          edata, ctx);
    });
    check(has(hollow, "catalog is empty"),
          "an empty catalog is reported as an empty catalog");
    check(has(hollow, "discoverActions"),
          "...and points at the step that did not happen");
    check(empty_ret == false, "...and returns false like any other miss");

    // -- 6. the report is one function ----------------------------------------
    //
    // Both paths carry the tag-block sentence. The point of the shared function
    // is that a future change -- recording the attempt rather than logging it --
    // lands on both without anyone remembering the second one exists.
    check(has(streamed, "ETCS_TAG_BLOCK") && has(hollow, "ETCS_TAG_BLOCK"),
          "every path through the report says the same thing");

    e->call("Layout.Delete", "", ctx);
    ETCS::PendingUnloadRegistry::getInstance().join_all();

    std::printf("\n%s (%d failure%s)\n", g_fail ? "FAILED" : "PASSED",
                g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
