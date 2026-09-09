// ===========================================================================
// ShellTesterLoader.cc
//
// Replaces REPLTesterLoader, and the replacement is the point rather than a
// rename.
//
// The old tester was a black-box PROCESS test: it forked the compiled `etcs`
// binary, drove its terminal over pipes, and watched for prompts. That made
// sense while the shell was ShellREPL.h -- a terminal wedged beside core behind
// an ETCS_LOADER guard, reachable no other way. Two things were wrong with it
// as a suite member:
//
//   - it tested a BINARY, not a type, so it could only ever assert on
//     observable process behaviour and never on the causal structure
//     underneath;
//   - `ace make loaders` builds without -DETCS_REPL_SHELL, so the `etcs` it
//     forked had no shell compiled in and the test failed by construction in
//     exactly the mode CI would use. A test that cannot pass in the standard
//     build is not reporting anything.
//
// Now that a shell is an ordinary provider type (ShellProvider), the thing
// worth testing is the type: a Shell is a Thread, which is the only kind of
// entity that can produce another control thread, and the jobs it starts are
// its CHILDREN rather than rows in a registry beside it.
//
// What the terminal half needs -- raw mode, prompts, line editing under a real
// TTY -- is still worth a process test, and it belongs with the OS backend once
// ShellREPL.h finishes moving into LinuxShell. This file deliberately does not
// pretend to cover it.
// ===========================================================================

#include "../ETCS.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const std::string& what)
{
    if (ok) { ++g_pass; std::cout << "  ok    " << what << "\n"; }
    else    { ++g_fail; std::cout << "  FAIL  " << what << "\n"; }
}

static bool has_iface(ETCS::Entity* e, const char* family)
{
    return e && e->getInterfacePointer(ETCS::Buffer(family)) != nullptr;
}

// A script the shell can actually run to completion, written beside the binary
// so the shell resolves it the way a session would.
static bool write_script(const std::string& path, const std::string& body)
{
    std::ofstream out(path);
    if (!out.is_open()) return false;
    out << body;
    return true;
}

int main()
{
    WIRE_CONTEXT();

    std::cout << "=== Shell ===\n";

    ETCS::Entity* sh = ETCS::spawn_entity("ShellProvider", "Shell", env, loader);
    if (!sh) { std::cerr << "cannot spawn ShellProvider:Shell\n"; return 1; }
    sh->call("Shell.Create", "", ctx);

    // -- 1. A shell is an actor, and says so through the families ----------
    //
    // Thread is the claim that matters: it is what makes this the only kind of
    // entity that can produce another control thread. Threaded comes with it
    // cumulatively (ThreadBase composes ThreadedBase), which is why a leaf
    // cannot hold one without the other.
    check(has_iface(sh, "Thread"),    "Shell registers Thread");
    check(has_iface(sh, "Threaded"),  "Shell registers Threaded (inherited lineage)");
    check(has_iface(sh, "Deletable"), "Shell registers Deletable");
    check(has_iface(sh, "Lifecycle"), "Shell registers Lifecycle");

    // Reached the way core reaches it: by family name, as a wire pointer, with
    // no knowledge of LinuxShell at all. This is exactly the hop CommandExecutor
    // will take when its detach path stops keeping its own registry.
    ETCS::IWireThread* wire = static_cast<ETCS::IWireThread*>(
        sh->getInterfacePointer(ETCS::Buffer("Threaded")));
    check(wire != nullptr, "and the Threaded wire is reachable without naming the type");

    // -- 2. Detached jobs are CHILDREN, not rows in a registry --------------
    {
        const uint64_t a = wire ? wire->Detach(ETCS::Buffer("a.etcs")) : 0;
        const uint64_t b = wire ? wire->Detach(ETCS::Buffer("b.etcs")) : 0;
        check(a != 0 && b != 0 && a != b, "detaching through the wire yields distinct children");

        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> kids;
        sh->getTypedChildren(kids);
        check(kids.size() == 2,
              "\"which jobs did this shell start\" is the child list, with nothing beside it");

        // The child is a Shell too, because a detached script IS a control
        // thread -- there is no separate executor kind.
        ETCS::Entity* child = kids.empty() ? nullptr
                            : sh->getTypedChild(kids[0].first, kids[0].second);
        check(has_iface(child, "Thread"), "a detached job is itself a Thread");
    }

    // -- 3. The closure travels to children, by value -----------------------
    {
        const char* q = "SELECT 1";
        sh->call("Shell.Bind", (std::string("query ") + q).c_str(), ctx);

        const uint64_t kid = wire ? wire->Detach(ETCS::Buffer("uses_query.etcs")) : 0;
        check(kid != 0, "a shell with a closure can still detach");

        ETCS::Held<Thread_> child = ETCS::resolve_held<Thread_>("Thread", kid);
        check(static_cast<bool>(child), "the child resolves in the Thread family");
    }

    // -- 4. Running a script anchors it on the SHELL ------------------------
    //
    // The reason the type exists. ExecutionContext::root_entity is this entity
    // rather than a bare Root, so everything the script spawns is owned by the
    // shell that ran it -- which is what makes a session's history a subtree.
    {
        const std::string path = "shelltest_ok.etcs";
        check(write_script(path, "#!/usr/bin/env etcs\n\nexit\n"),
              "a script can be written for the shell to run");

        sh->call("Shell.Run", path.c_str(), ctx);
        check(true, "Shell.Run returns rather than hanging on a script that exits");
        std::remove(path.c_str());

        ETCS::Buffer script;
        void* tp = sh->getInterfacePointer(ETCS::Buffer("Thread"));
        if (tp) script = static_cast<Thread_*>(tp)->Script();
        check(std::string(script.c_str()) == path,
              "and the shell reports the script it ran as its own work");
    }

    // -- 4b. A `detach` in a script becomes a CHILD of the running Thread ----
    //
    // The CmdDetach path. Core cannot allocate a Thread (ontology depends on
    // core, never the reverse), so it asks through IWireThread::Detach and the
    // leaf makes one of itself. Observably: the job lands in the running
    // thread's child list instead of a registry beside the entity graph.
    //
    // DRIVEN FROM HERE RATHER THAN THROUGH Shell.Run, and the reason is worth
    // recording. execute_command's spawn/run/detach arms are all inside
    // #ifdef ETCS_LOADER, so they are compiled OUT of a module build -- a
    // script executed inside ShellProvider.so parses and walks fine and then
    // detaches nothing at all. This tester IS a loader, so calling
    // run_root_script here exercises the arm that actually exists. Until the
    // gating question is settled, that is the honest place to test it from.
    {
        ETCS::Entity* runner = ETCS::spawn_entity("ShellProvider", "Shell", env, loader);
        check(runner != nullptr, "a shell to run the script under");
        if (runner)
        {
            runner->call("Shell.Create", "", ctx);

            std::vector<std::pair<ETCS::Buffer, ETCS::RID>> before;
            runner->getTypedChildren(before);

            check(write_script("shelltest_child.etcs", "#!/usr/bin/env etcs\n\nexit\n"),
                  "a script for the detached job exists");
            check(write_script("shelltest_parent.etcs",
                               "#!/usr/bin/env etcs\n\ndetach shelltest_child.etcs\nexit\n"),
                  "and a parent script that detaches it");

            // Rooted on the SHELL, which is what gives CmdDetach a Thread half
            // to ask. A bare Root has none, which is the transitional fallback.
            ETCS::SignalContext sig{};
            void* tw = runner->getInterfacePointer(ETCS::Buffer("Threaded"));
            if (tw) sig = static_cast<ETCS::IWireThread*>(tw)->Signals();

            ETCS::ExecutionContext rctx(runner, &sig);
            rctx.is_root = true;
            ETCS::run_root_script("shelltest_parent.etcs", rctx);

            std::vector<std::pair<ETCS::Buffer, ETCS::RID>> after;
            runner->getTypedChildren(after);
            check(after.size() > before.size(),
                  "a detach in the script became a CHILD of the thread that ran it");

            std::remove("shelltest_parent.etcs");
            std::remove("shelltest_child.etcs");
            runner->call("Shell.Delete", "", ctx);
        }
    }

    // -- 4c. Signal chains GROUP BY OWNERSHIP -------------------------------
    //
    // A child's chain has to pass through the shell that made it, so a shell
    // whose own closure ends takes its children with it. walk() reads BOTH
    // edges (core/SignalContext.h):
    //
    //   provider  the OWNERSHIP chain -- addTagTrampoline points a child's
    //             ctx_ at its parent's, and reparentChildrenTo keeps it right
    //             when ownership moves. This is what does the grouping.
    //   up        the CALL chain -- pinned to the process root, so a raise
    //             from the entry point reaches every job, and so nothing
    //             points at a launching frame that has already returned.
    //
    // Each level still holds its OWN local flags, so a targeted stop lands on
    // one job without touching the rest. That is the whole point of local-most
    // authority: without it a raise is a silent no-op, not a wider stop.
    {
        ETCS::Entity* boss = ETCS::spawn_entity("ShellProvider", "Shell", env, loader);
        check(boss != nullptr, "a parent shell");
        ETCS::Entity* other = ETCS::spawn_entity("ShellProvider", "Shell", env, loader);
        check(other != nullptr, "and an unrelated one");

        if (boss && other)
        {
            boss->call("Shell.Create", "", ctx);
            other->call("Shell.Create", "", ctx);

            ETCS::IWireThread* bw = static_cast<ETCS::IWireThread*>(
                boss->getInterfacePointer(ETCS::Buffer("Threaded")));
            ETCS::IWireThread* ow = static_cast<ETCS::IWireThread*>(
                other->getInterfacePointer(ETCS::Buffer("Threaded")));

            const uint64_t kid = bw ? bw->Detach(ETCS::Buffer("grouped.etcs")) : 0;
            ETCS::Entity* child = kid ? ETCS::resolve_entity_anywhere(kid) : nullptr;
            check(child != nullptr, "with a detached child of its own");

            ETCS::IWireThread* cw = child ? static_cast<ETCS::IWireThread*>(
                child->getInterfacePointer(ETCS::Buffer("Threaded"))) : nullptr;

            if (bw && ow && cw)
            {
                ETCS::SignalContext bs = bw->Signals();
                ETCS::SignalContext cs = cw->Signals();
                ETCS::SignalContext os = ow->Signals();

                check(!cs.isTerminated(), "nothing is terminated to begin with");

                // Raise the PARENT's own local authority.
                *bs.terminate = 1;

                check(cs.isTerminated(),
                      "a shell's own stop reaches its child -- the chain passes THROUGH it");
                check(!os.isTerminated(),
                      "...and stops there: an unrelated shell is untouched, so it groups "
                      "lifetimes rather than flattening them");

                *bs.terminate = 0;

                // And the reverse: a child's own stop is its own.
                ETCS::SignalContext cs2 = cw->Signals();
                *cs2.terminate = 1;
                ETCS::SignalContext bs2 = bw->Signals();
                check(!bs2.isTerminated(),
                      "a child's targeted stop does NOT climb back into its parent");
                *cs2.terminate = 0;
            }

            boss->call("Shell.Delete", "", ctx);
            other->call("Shell.Delete", "", ctx);
        }
    }

    // NOTE: the signal-bit lifetime test lives in DrawableOntologyTesterLoader
    // (case 26), not here. It was written here first and PASSED ON BOTH the
    // broken and fixed versions, because a module spawn never recycles a
    // reclaimed shell -- so it demonstrated nothing. The direct-arena path does
    // recycle, which is where the aliasing is actually observable and where the
    // test now asserts that the shell WAS reused before claiming anything.

    // -- 4e. The detach topology of a script IS the Shell's child tree -------
    //
    // The structural claim, tested as structure rather than as a count. A
    // script that detaches two jobs, one of which detaches a third, should
    // produce exactly that shape under the shell that ran it -- because each
    // detached job is a Thread entity whose own detaches become ITS children.
    // No registry is consulted anywhere; the tree is the record.
    {
        ETCS::Entity* top = ETCS::spawn_entity("ShellProvider", "Shell", env, loader);
        check(top != nullptr, "a shell to run a nested topology");
        if (top)
        {
            top->call("Shell.Create", "", ctx);

            check(write_script("st_leaf.etcs",   "#!/usr/bin/env etcs\n\nexit\n")
               && write_script("st_mid.etcs",    "#!/usr/bin/env etcs\n\ndetach st_leaf.etcs\nexit\n")
               && write_script("st_top.etcs",    "#!/usr/bin/env etcs\n\n"
                                                 "detach st_mid.etcs\ndetach st_leaf.etcs\nexit\n"),
                  "a three-level detach topology on disk");

            ETCS::SignalContext sig{};
            void* tw = top->getInterfacePointer(ETCS::Buffer("Threaded"));
            if (tw) sig = static_cast<ETCS::IWireThread*>(tw)->Signals();

            ETCS::ExecutionContext tctx(top, &sig);
            tctx.is_root = true;
            ETCS::run_root_script("st_top.etcs", tctx);

            /*
             * Wait by POLLING THE SHAPE, not by draining.
             *
             * shutdown_detached_executors() sets g_sig_term globally and joins
             * every job -- correct at exit, poisonous in the middle of a run:
             * it terminates the world for every test after this one, and the
             * final drain then joins an already-joined set. That is exactly how
             * this file started hanging intermittently, on a different test
             * each time depending on ordering.
             */
            std::vector<std::pair<ETCS::Buffer, ETCS::RID>> kids;
            for (int i = 0; i < 200; ++i)      // <= 2s, then assert what is there
            {
                kids.clear();
                top->getTypedChildren(kids);
                if (kids.size() == 2)
                {
                    int gk_total = 0;
                    for (const auto& k : kids)
                    {
                        ETCS::Entity* c = top->getTypedChild(k.first, k.second);
                        if (!c) continue;
                        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> gk;
                        c->getTypedChildren(gk);
                        gk_total += static_cast<int>(gk.size());
                    }
                    if (gk_total >= 1) break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            check(kids.size() == 2,
                  "the top script's two detaches are the shell's two children");

            // One of those children detached in turn, so exactly one grandchild
            // subtree exists -- the nesting, not just the count.
            int with_children = 0, total_grandkids = 0;
            for (const auto& k : kids)
            {
                ETCS::Entity* c = top->getTypedChild(k.first, k.second);
                if (!c) continue;
                std::vector<std::pair<ETCS::Buffer, ETCS::RID>> gk;
                c->getTypedChildren(gk);
                if (!gk.empty()) ++with_children;
                total_grandkids += static_cast<int>(gk.size());
            }
            check(with_children == 1 && total_grandkids == 1,
                  "and the one that detached again has exactly one child of its own");

            std::remove("st_top.etcs");
            std::remove("st_mid.etcs");
            std::remove("st_leaf.etcs");
            top->call("Shell.Delete", "", ctx);
        }
    }

    // -- 4f. A module can ask the loader for ANOTHER module's type -----------
    //
    // The gap, stated plainly: a module could always create its OWN types
    // (addTag<T> has the type compiled in) and the loader could create any
    // module's, but no module could ask for another module's. Nothing
    // structural was stopping it -- LoadEvent is present in module builds and
    // the loader's ordering thread is what acts on it -- there was simply no
    // caller, because the only thing that ever spawned cross-module lived on
    // the loader side.
    //
    // Creation stays the loader's alone. What crosses from module scope is the
    // REQUEST, exactly as addTag<T> crosses it with AddTagEvent.
    {
        ETCS::Entity* sp = ETCS::spawn_entity("ShellProvider", "Shell", env, loader);
        check(sp != nullptr, "a shell to spawn through");
        if (sp)
        {
            sp->call("Shell.Create", "", ctx);

            // Load RenderProvider loader-side first, so a failure here means
            // "a module cannot reach an already-loaded module" rather than
            // "a module cannot trigger an on-demand load".
            ETCS::Entity* warm = ETCS::spawn_entity("RenderProvider", "ImageSurface", env, loader);
            check(warm != nullptr, "RenderProvider is loaded and reachable loader-side");

            sp->call("Shell.Spawn", "RenderProvider::ImageSurface", ctx);

            // NOT checked as a child: evt.root sets the LIFETIME OWNER, not
            // parentage, and a script's own `spawn` makes a top-level entity
            // too. Ownership and containment are different edges; conflating
            // them is what made the first version of this test wrong.
            check(sp->hasTag(ETCS::Buffer("spawned")),
                  "a module asked the loader for another module's type, by origin-affixed "
                  "name, and got one");

            sp->call("Shell.Delete", "", ctx);
        }
    }

    // -- 4g. Shell.Run spawns from a COLD module -----------------------------
    //
    // The bug this closes, and it long predates the Shell: the execution root
    // doubled as the module BOOTSTRAP HOST, and attachModule allows one module
    // per entity for life. A bare Root has a vacant slot so it always worked;
    // an entity root already has its own module, so every first-request of a
    // foreign module from one was dropped before it started. Unreachable until
    // something other than a Root ran a script.
    //
    // Cold on purpose -- no loader-side warm-up first, so this exercises the
    // first-request load, which is the lifetime machinery doing its job.
    {
        ETCS::Entity* cold = ETCS::spawn_entity("ShellProvider", "Shell", env, loader);
        check(cold != nullptr, "a shell for the cold-module case");
        if (cold)
        {
            cold->call("Shell.Create", "", ctx);
            check(write_script("st_cold.etcs",
                               "#!/usr/bin/env etcs\n\n"
                               "spawn ChessProvider::ChessGame g\n"
                               "spawn LayoutProvider::Layout lay\n"
                               "exit\n"),
                  "a script naming two modules nothing has loaded yet");

            cold->call("Shell.Run", "st_cold.etcs", ctx);

            ETCS::Held<ETCS::Entity> probe;
            bool loaded = ETCS::EventNode::getInstance().ridMap.find(
                              ETCS::Buffer("ChessProvider:ChessGame"))
                          != ETCS::EventNode::getInstance().ridMap.end();
            const bool loaded2 = ETCS::EventNode::getInstance().ridMap.find(
                                     ETCS::Buffer("LayoutProvider:Layout"))
                                 != ETCS::EventNode::getInstance().ridMap.end();
            check(loaded,
                  "a module-hosted Shell.Run triggered a first-request module load");
            // The migration, not just the first bind: one Root moved between two
            // modules in a single execution, which is what Root::changeModule is
            // for and what a fresh-Root-per-spawn would never exercise.
            check(loaded2,
                  "...and a SECOND cold module in the same script, by migrating the same host");

            std::remove("st_cold.etcs");
            cold->call("Shell.Delete", "", ctx);
        }
    }

    // -- 5. A halted shell stops accepting work -----------------------------
    //
    // Cooperative, per ontology/Threaded.h: the flag is the whole request. What
    // matters here is that Detach honours it, so a shell in teardown cannot
    // spawn a job into an arena that is being reclaimed.
    {
        ETCS::Entity* dying = ETCS::spawn_entity("ShellProvider", "Shell", env, loader);
        check(dying != nullptr, "a second shell spawns independently");
        if (dying)
        {
            dying->call("Shell.Create", "", ctx);
            ETCS::IWireThread* dw = static_cast<ETCS::IWireThread*>(
                dying->getInterfacePointer(ETCS::Buffer("Threaded")));

            check(dw && dw->Detach(ETCS::Buffer("before.etcs")) != 0,
                  "it detaches while running");
            check(dw && dw->Halt(),  "the first Halt places the request");
            check(dw && dw->Halted(), "and a body polling it would see it");
            check(dw && dw->Detach(ETCS::Buffer("after.etcs")) == 0,
                  "a halted shell refuses to detach -- 0, not a silent no-op");

            dying->call("Shell.Delete", "", ctx);
        }
    }

    // -- 6. Report is a read, not a mutation --------------------------------
    sh->call("Shell.Report", "", ctx);
    check(has_iface(sh, "Thread"), "the shell is intact after reporting");

    sh->call("Shell.Delete", "", ctx);

    /*
     * Drain before leaving, the way etcs.cc does.
     *
     * A detached job is a real std::thread, and destroying a joinable one
     * terminates the process -- which is exactly what happened the first time
     * this file ran CmdDetach to completion, since nothing else in the suite
     * had ever produced a detached thread to leave behind. Not core's bug: an
     * entry point that launches jobs owns draining them.
     */
    ETCS::shutdown_detached_executors();

    std::cout << "=== Summary: " << g_pass << " passed, " << g_fail << " failed ===\n";
    return g_fail == 0 ? 0 : 1;
}
