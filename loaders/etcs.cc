// ETCS_REPL_SHELL is now a BUILD-CONTROLLED switch, not hardcoded here --
// pass -DETCS_REPL_SHELL on the compiler command line for the interactive
// binary target; omit it for a non-interactive "environment" binary that
// runs an initial script, then blocks until every `detach`ed job it
// spawned has finished (see wait_for_environment_drain, CommandExecutor.h)
// instead of dropping to a prompt. Both binaries compile from this exact
// same source file -- only the flag differs.
//
// IMPORTANT, BUILD-SIDE: this file used to #define ETCS_REPL_SHELL
// unconditionally, right here, meaning no Makefile rule building this file
// was ever actually supplying the flag itself -- the behavior was fixed at
// source level regardless of build target. Now that the #define is gone,
// whichever Makefile target is meant to keep today's interactive behavior
// (almost certainly whatever currently produces the `etcs` binary) MUST
// have -DETCS_REPL_SHELL added to its own compile flags, or it will
// silently switch to non-interactive drain-mode instead. A NEW target,
// without that flag, is what actually gets you the daemon/environment
// binary this change was for.
#undef ETCS_PRODUCTION_BUILD
#include "../ETCS.h"
#include <fstream>
#include <iostream>
#include <string>

// COLOR_WARN/COLOR_RESET (and every other COLOR_* macro) now come
// unconditionally from ShellREPL.h, which ETCS.h always includes in a
// loader build regardless of ETCS_REPL_SHELL -- real ANSI codes under
// the interactive build, empty strings under drain mode. No fallback
// needed here anymore.

int main(int argc, char* argv[])
{
    shell_startup();
    WIRE_CONTEXT();
    // drive_main_loop_then_exit (ShellREPL.h) is what every path through
    // main() funnels through so that, once whichever top-level loop
    // applies actually returns -- the user leaving the REPL (`exit`/
    // `quit`, or Ctrl+C breaking repl_shell_loop's own signal check), or
    // wait_for_environment_drain unblocking (every detached job finished,
    // or an interrupt/terminate signal) -- any still-running `detach`ed
    // background scripts get signalled to stop and are joined before the
    // process exits, exactly once, regardless of which branch ran. It
    // takes ctx explicitly now rather than closing over it, since it no
    // longer lives here as a local lambda.
    if (argc < 2)
    {
        // ── No script given ───────────────────────────────────────────────────
        // REPL build: straight to the interactive prompt, as before.
        // Drain build: nothing was ever run, so DetachedRegistry is empty and
        // wait_for_environment_drain returns immediately (see its own comment,
        // CommandExecutor.h, on why an empty registry is correct-and-trivial,
        // not an error) -- this is a legitimate, if uninteresting, no-op.
        return drive_main_loop_then_exit(ctx, 0);
    }
    // ── Script file mode ──────────────────────────────────────────────────────
    // Usage: etcs <script.etcs> [name=RID ...]
    // Example:
    //   etcs migrate.etcs source=42 dest=97
    // Check for quiet mode flag
    if (argc >= 2 && (std::string_view(argv[1]) == "-q" || std::string_view(argv[1]) == "--quiet"))
    {
        if (argc < 3) {
            std::cerr << COLOR_WARN << "etcs: -q requires a script file argument" << COLOR_RESET << "\n";
            return 1;
        }
        ETCS::log_enabled.store(false, std::memory_order_relaxed);
        // Shift args down
        argv++;
        argc--;
    }
    // ── Control socket ────────────────────────────────────────────────────
    // etcs --listen <socket> [script.etcs] [name=RID ...]
    //
    // Flag-first, matching -q's own convention above. With a socket the
    // headless build accepts control sessions instead of draining, which is
    // also what keeps a server process alive past the trace that started it
    // (see run_control_listener, CommandExecutor.h).
    std::string listen_path;
    if (argc >= 2 && std::string_view(argv[1]) == "--listen")
    {
        if (argc < 3)
        {
            std::cerr << COLOR_WARN << "etcs: --listen requires a socket path"
                      << COLOR_RESET << "\n";
            return 1;
        }
        listen_path = argv[2];
        argv += 2;
        argc -= 2;
    }

    // A socket with no script is legitimate: an empty runtime that waits to
    // be told what to do.
    if (argc < 2)
        return drive_main_loop_then_exit(ctx, 0, listen_path);

    const std::string filepath = argv[1];

    std::ifstream file(filepath);
    if (!file.is_open())
    {
        std::cerr << COLOR_WARN << "etcs: cannot open script file '"
                  << filepath << "'" << COLOR_RESET << "\n";
        return drive_main_loop_then_exit(ctx, 1);
    }

    ETCS::ExecutionContext script_ctx;
    script_ctx.sig = &ctx;
    script_ctx.root_entity = &root;

    // Parse name=RID injection arguments
    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        auto eq = arg.find('=');
        if (eq == std::string::npos || eq == 0 || eq == arg.size() - 1)
        {
            std::cerr << COLOR_WARN << "etcs: invalid injection argument '"
                      << arg << "' -- expected name=RID" << COLOR_RESET << "\n";
            return drive_main_loop_then_exit(ctx, 1);
        }
        std::string name    = arg.substr(0, eq);
        std::string rid_str = arg.substr(eq + 1);
        // Validate name
        bool valid_name = true;
        for (char c : name)
            if (!std::isalnum((unsigned char)c) && c != '_') { valid_name = false; break; }
        if (!valid_name || name.empty())
        {
            std::cerr << COLOR_WARN << "etcs: invalid name '" << name
                      << "' in injection argument '" << arg << "'" << COLOR_RESET << "\n";
            return drive_main_loop_then_exit(ctx, 1);
        }
        // Parse RID
        try {
            size_t end;
            unsigned long long rid = std::stoull(rid_str, &end);
            if (end != rid_str.size()) throw std::invalid_argument("trailing chars");
            
            // Resolved HERE, not deferred. A binding carries its
            // Module::Tag now (action lines no longer state one), and
            // an injected RID is the one place that pair is not
            // already known -- so it is recovered from the entity
            // itself, which also settles liveness at capture time.
            ETCS::Entity* e = ETCS::resolve_entity_anywhere(static_cast<ETCS::RID>(rid));
            if (!e)
            {
                std::cerr << COLOR_WARN << "etcs: RID " << rid
                          << " (for '" << name << "') does not resolve to a "
                             "live entity." << COLOR_RESET << "\n";
                return drive_main_loop_then_exit(ctx, 1);
            }
            script_ctx.bind(name, ETCS::NameBinding{
                static_cast<ETCS::RID>(rid), 
                e->getSourceModule().toString(), 
                e->getSourceTag().toString()
            });
            ETCS_LOG("ETCS", "Injected: " << name << " -> RID:" << rid
                     << " (" << e->getSourceModule().toString()
                     << "::" << e->getSourceTag().toString() << ")");
        }
        catch (...) {
            std::cerr << COLOR_WARN << "etcs: invalid RID '" << rid_str
                      << "' in injection argument '" << arg << "'" << COLOR_RESET << "\n";
            return drive_main_loop_then_exit(ctx, 1);
        }
    }

    // Run the script. If the user hits Ctrl+C, global_signal_handler sets 
    // g_sig_int = 1, which causes blocking loops (like Listen) to gracefully break.
    //
    // run_script returning here means only THIS script's own lines are
    // exhausted — any `detach`ed background scripts it launched keep running
    // independently (see drive_main_loop_then_exit above and the comment on
    // run_script's return path in CommandExecutor.h). That's what lets a
    // script that detaches a long-running server and has nothing left to do
    // fall straight through to whichever top-level loop applies below,
    // instead of blocking here waiting for the server to stop -- on a REPL
    // build that's the interactive shell; on a drain build that's
    // wait_for_environment_drain, which is exactly the gserver.etcs shape
    // this whole switch was added for: run the two `detach` lines, then
    // block on the environment they set up, no prompt involved at all.
    ETCS::run_script(file, filepath, script_ctx);
    // Clear the interrupt flag so the following loop doesn't instantly exit
    g_sig_int = 0;
    // Drop into whichever top-level loop applies after script completion or
    // interruption.
    return drive_main_loop_then_exit(ctx, 0, listen_path);
}