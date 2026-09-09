// ETCS_REPL_SHELL says what this binary should DO, not what compiles into it.
//
// With it: load the Shell provider, take the terminal it exports, and drop the
// operator into the navigator. Without it: run the initial script, then block
// until every `detach`ed job it spawned has finished (wait_for_environment
// _drain, CommandExecutor.h), or accept control sessions on --listen. Both
// binaries compile from this source file and contain the same code -- only the
// top-level loop differs.
//
// It USED to select code, back when the terminal was ShellREPL.h beside core.
// It no longer can: the terminal is ShellProvider.so, an ordinary module the
// loader dlopens, so an interactive runtime needs that file present at RUNTIME
// rather than a flag present at build time. A build carrying the flag with no
// provider to find says so and drains instead of prompting.
//
// BUILD-SIDE, unchanged: whichever Makefile target produces the interactive
// `etcs` must pass -DETCS_REPL_SHELL. A target without it is the
// daemon/environment binary.
#undef ETCS_PRODUCTION_BUILD
#include "../ETCS.h"
#include <fstream>
#include <iostream>
#include <string>

// COLOR_WARN/COLOR_RESET (and every other COLOR_* macro) come from
// core/CommandExecutor.h, which arrives with ETCS.h in any executor host.
// They are RUNTIME conditionals now rather than build-mode literals: real
// ANSI codes when stdout is a terminal, empty strings when it is a pipe or a
// file, in either binary. No fallback needed here.

int main(int argc, char* argv[])
{
    shell_startup();
    WIRE_CONTEXT();
    // drive_main_loop_then_exit (CommandExecutor.h) is what every path through
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
        // REPL build: load the Shell provider and prompt.
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