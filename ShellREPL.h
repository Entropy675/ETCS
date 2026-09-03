#ifndef SHELLREPL_H__
#define SHELLREPL_H__

#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstring>
#include <functional>
#include <filesystem>
#include <algorithm>
#include <unordered_set>
#include "ETCS.h"

// Real ANSI codes interactively; empty strings in drain mode, where output
// usually isn't a TTY and plain text is correct rather than degraded.
#ifdef ETCS_REPL_SHELL
#define COLOR_RESET   "\033[0m"
#define COLOR_DIR     "\033[1;36m"
#define COLOR_LIB     "\033[1;32m"
#define COLOR_WARN    "\033[1;33m"
#define COLOR_RID     "\033[1;35m"
#define COLOR_ACT     "\033[1;34m"
#define COLOR_EXEC    "\033[0;35m"
#else
#define COLOR_RESET   ""
#define COLOR_DIR     ""
#define COLOR_LIB     ""
#define COLOR_WARN    ""
#define COLOR_RID     ""
#define COLOR_ACT     ""
#define COLOR_EXEC    ""
#endif

// NOTE on ETCS_LOG: the macro expands to a bare `if`, so an unbraced
// ETCS_LOG as an if/else branch body swallows the following `else`. Every
// use in a branch position below is braced for that reason.

// ---------------------------------------------------------------------------
// THE GATING SPLIT
//
// Navigation is a RUNTIME CAPABILITY, gated on ETCS_LOADER. The terminal is
// one INPUT SOURCE for it, gated on ETCS_REPL_SHELL.
//
// The loops take a ReplLineSource and know nothing about where lines come
// from. Raw-mode input, history and tab completion stay terminal-only;
// everything that reads the entity graph is available wherever ETCS_LOADER
// is. A remote session is handed the SAME navigator the local console uses,
// rendered by the side that actually has the entity graph -- no proxying, no
// mirrored surface, because the process answering the questions is the one
// holding the answers.
//
// ---------------------------------------------------------------------------
// THE BROWSE / SCRIPT SPLIT
//
// This file is the BROWSE surface, and it no longer executes .etcs lines.
// A session used to be able to type raw trace lines at a prompt and have them
// interpreted by the same parser a file goes through. That is gone, and its
// absence is what makes the script grammar affordable.
//
// The two surfaces want opposite things. A script is written by someone who
// already knows the types, is read long after it ran, and benefits from a
// grammar that refuses everything it cannot resolve statically. Someone at a
// prompt is doing the opposite -- finding out what exists -- and every rule
// that makes a trace trustworthy makes exploration worse: naming a receiver
// you are still looking for, bracketing arguments to an action you have not
// found yet, declaring a type before you know which one you want.
//
// So the navigator keeps its own input vocabulary, which was never script
// syntax anyway: bare numbers select, `back`/`up` move, `c0` descends into a
// child, `s0` interrupts by position, and a bare action name dispatches. It
// builds Command values DIRECTLY and hands them to execute_command. What the
// two surfaces share is execution -- one implementation of what an action
// does -- not a parser for two grammars with different jobs.
// ---------------------------------------------------------------------------

// prompt in, line out. Returns false when the source is finished (peer
// closed, signal raised, stdin gone) -- every loop treats that as "leave".
using ReplLineSource = std::function<bool(const std::string& prompt, std::string& out)>;

// Output helpers. ETCS_LOG already follows ETCS::log_sink; these are for the
// handful of places that wrote to cout/cerr directly, which under a session
// would have gone to the SERVER's console rather than to whoever typed the
// command. thread_local sink, so a local terminal is unaffected and two
// concurrent sessions never cross-talk.
inline std::ostream& repl_out() { return ETCS::log_sink ? *ETCS::log_sink : std::cout; }
inline std::ostream& repl_err() { return ETCS::log_sink ? *ETCS::log_sink : std::cerr; }

/*
 * A REPLY IS NOT A LOG LINE, and until the destination became switchable
 * nothing had to say so.
 *
 * These two were the same stream, which was harmless while the log had exactly
 * one place to go. The moment `log file` existed it stopped being harmless: the
 * shell answered `jobs` into logs/etcs.log and left the person who typed it
 * looking at a bare prompt. The output they asked to move is the PROVIDERS'
 * chatter, and their own command's answer is the one thing that must never
 * follow it.
 *
 * So the shell's replies go to repl_out() unconditionally -- which still
 * honours log_sink, because a session that borrowed this shell is the one
 * caller that genuinely is somewhere else. Same formatting as ETCS_LOG, on
 * purpose: it is the same shell, not a new voice.
 */
#define ETCS_SHELL(type, msg) ETCS_LOG_LINE(type, msg, repl_out())

inline bool repl_is_module(const std::filesystem::directory_entry& entry)
{
    auto ext = entry.path().extension().string();
    return (ext == ".so" || ext == ".dll" || ext == ".dylib");
}

inline bool repl_iequals_prefix(const std::string& full, const std::string& partial)
{
    if (partial.size() > full.size()) return false;
    return std::equal(partial.begin(), partial.end(), full.begin(),
                      [](char a, char b) {
                          return std::tolower((unsigned char)a)
                              == std::tolower((unsigned char)b);
                      });
}

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Terminal input source -- raw mode, history, tab completion. Everything in
// this block is genuinely tty-specific and stays compiled out of a drain
// build; the navigator does not reference any of it.
// ---------------------------------------------------------------------------
#ifdef ETCS_REPL_SHELL

#if defined(_WIN32) || defined(_WIN64)
    #include <conio.h>
    #include <windows.h>
    #define GETCH _getch
    inline void repl_shell_enable_vterm() {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, dwMode);
        }
    }
#else
    #include <sys/ioctl.h>
    #include <termios.h>
    #include <unistd.h>
    #include <poll.h>
    #include <thread>
    #include <chrono>
    #define GETCH repl_shell_get_char_unix

    // Non-TTY branch polls with a timeout instead of blocking on read().
    // Deliberately local rather than clearing SA_RESTART process-wide --
    // other blocking calls (network I/O inside a `run` script) depend on
    // auto-retry to forward an interrupt to the executing context rather
    // than aborting on the first EINTR. A TTY never needed this: its line
    // discipline already wakes a blocked read on SIGINT.
    //
    // Returns 0 on timeout AND on read failure/closed pipe (the sleep avoids
    // spinning on a dead pipe, which poll() reports ready forever via
    // POLLHUP). Callers treat 0 as "no character, re-check signals".
    inline char repl_shell_get_char_unix() {
        char buf = 0;
        if (!isatty(STDIN_FILENO)) {
            struct pollfd pfd{ STDIN_FILENO, POLLIN, 0 };
            int pr = poll(&pfd, 1, 100); // 100ms
            if (pr <= 0) return 0;
            ssize_t n = read(STDIN_FILENO, &buf, 1);
            if (n <= 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                return 0;
            }
            return buf;
        }
        struct termios old;
        std::memset(&old, 0, sizeof(struct termios));
        if (tcgetattr(STDIN_FILENO, &old) < 0) return 0;
        struct termios raw = old;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) return 0;
        if (read(STDIN_FILENO, &buf, 1) < 0) buf = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        return buf;
    }
    inline void repl_shell_enable_vterm() {}
#endif

inline std::vector<std::string> g_session_history;
inline const std::string HISTORY_FILE = ".etcs_history";
static constexpr size_t MAX_HISTORY_BYTES = 16 * 1024;

// These are ETCS::SignalFlag (std::atomic), so `g_sig_int = 0` compiles as a
// seq_cst store and `if (g_sig_int)` as a seq_cst load -- correct but
// stronger than needed, and silently so. Named once instead of spelling the
// ordering out at each site.
inline void repl_clear_signal_flags()
{
    g_sig_int .store(0, std::memory_order_release);
    g_sig_term.store(0, std::memory_order_release);
    g_sig_usr1.store(0, std::memory_order_release);
}

inline bool repl_sigint_raised()
{
    return g_sig_int.load(std::memory_order_acquire) != 0;
}

inline void repl_shell_prune_history()
{
    size_t total = 0;
    for (const auto& line : g_session_history) total += line.size() + 1;
    size_t drop_from_front = 0;
    while (total > MAX_HISTORY_BYTES && drop_from_front < g_session_history.size())
    {
        total -= g_session_history[drop_from_front].size() + 1;
        ++drop_from_front;
    }
    if (drop_from_front > 0)
        g_session_history.erase(g_session_history.begin(),
                                g_session_history.begin() + static_cast<long>(drop_from_front));
}

inline void repl_shell_load_history()
{
    g_session_history.clear();
    std::ifstream hfile(HISTORY_FILE);
    std::string line;
    while (std::getline(hfile, line))
        if (!line.empty()) g_session_history.push_back(line);
    repl_shell_prune_history();
}

inline void repl_shell_save_history()
{
    repl_shell_prune_history();
    std::ofstream hfile(HISTORY_FILE);
    for (const auto& line : g_session_history) hfile << line << "\n";
}

inline std::string repl_shell_get_input(const std::string& prompt, ETCS::SignalContext& ctx)
{
    std::string input;
    std::cout << prompt << std::flush;

    std::vector<std::string> current_matches;
    int cycle_index   = -1;
    int history_index = static_cast<int>(g_session_history.size());
    std::string original_partial, prefix_before_partial;

    auto refresh_line = [&](const std::string& new_text) {
        std::cout << "\r" << std::string(prompt.size() + input.size() + 5, ' ') << "\r";
        std::cout << prompt << new_text << std::flush;
        input = new_text;
    };

    while (true)
    {
        if (ctx.isInterrupted() || ctx.isTerminated()) break;
        int c = GETCH();
        if (ctx.isInterrupted() || ctx.isTerminated()) break;

#if defined(_WIN32)
        if (c == 0 || c == 224)
        {
            int spec = GETCH();
            if (spec == 72 && history_index > 0)
                { history_index--; refresh_line(g_session_history[history_index]); }
            else if (spec == 80)
            {
                if (history_index < (int)g_session_history.size() - 1)
                    { history_index++; refresh_line(g_session_history[history_index]); }
                else
                    { history_index = g_session_history.size(); refresh_line(""); }
            }
            continue;
        }
#else
        if (c == 27)
        {
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) > 0 && read(STDIN_FILENO, &seq[1], 1) > 0)
            {
                if (seq[0] == '[' && seq[1] == 'A' && history_index > 0)
                    { history_index--; refresh_line(g_session_history[history_index]); }
                else if (seq[0] == '[' && seq[1] == 'B')
                {
                    if (history_index < (int)g_session_history.size() - 1)
                        { history_index++; refresh_line(g_session_history[history_index]); }
                    else
                        { history_index = static_cast<int>(g_session_history.size()); refresh_line(""); }
                }
            }
            continue;
        }
#endif
        // 0 is a poll timeout (routinely, ~every 100ms while idle) or a read
        // failure -- never a keystroke. Looping back re-checks the signals at
        // the top; falling through would append a NUL on every idle cycle.
        if (c == 0) continue;

        if (c == '\r' || c == '\n')
        {
            std::cout << std::endl;
            if (!input.empty()
                && (g_session_history.empty() || input != g_session_history.back()))
            {
                g_session_history.push_back(input);
                repl_shell_save_history();
            }
            break;
        }
        else if (c == 8 || c == 127)
        {
            if (!input.empty()) { input.pop_back(); std::cout << "\b \b" << std::flush; }
            cycle_index = -1;
        }
        else if (c == '\t')
        {
            if (cycle_index == -1)
            {
                size_t last_space = input.find_last_of(" \t");
                prefix_before_partial = (last_space == std::string::npos)
                    ? "" : input.substr(0, last_space + 1);
                original_partial = (last_space == std::string::npos)
                    ? input : input.substr(last_space + 1);
                current_matches.clear();
                std::vector<std::string> exact, case_insens;
                try {
                    for (const auto& entry : fs::directory_iterator("."))
                    {
                        std::string name = entry.path().filename().string();
                        std::string cand = entry.is_directory()
                            ? name + "/"
                            : (repl_is_module(entry) ? entry.path().stem().string() : name);
                        if (cand.size() >= original_partial.size()
                            && cand.substr(0, original_partial.size()) == original_partial)
                            exact.push_back(cand);
                        else if (repl_iequals_prefix(cand, original_partial))
                            case_insens.push_back(cand);
                    }
                } catch (...) {}
                current_matches = exact.empty() ? case_insens : exact;
                std::sort(current_matches.begin(), current_matches.end());
                if (!current_matches.empty()) cycle_index = 0;
            }
            else
                cycle_index = (cycle_index + 1) % static_cast<int>(current_matches.size());

            if (cycle_index != -1)
            {
                size_t word_len = input.size() - prefix_before_partial.size();
                for (size_t i = 0; i < word_len; ++i) std::cout << "\b \b";
                input = prefix_before_partial + current_matches[cycle_index];
                std::cout << current_matches[cycle_index] << std::flush;
            }
        }
        else
        {
            input += static_cast<char>(c);
            std::cout << static_cast<char>(c) << std::flush;
            cycle_index = -1;
        }
    }
    return input;
}

// The tty as a ReplLineSource. Ends the loop it drives when a signal lands,
// which is what the old inline isInterrupted() checks after each read did.
inline ReplLineSource repl_tty_line_source(ETCS::SignalContext& sig)
{
    return [&sig](const std::string& prompt, std::string& out) -> bool
    {
        if (sig.isInterrupted() || sig.isTerminated()) return false;
        out = repl_shell_get_input(prompt, sig);
        return !(sig.isInterrupted() || sig.isTerminated());
    };
}

#endif // ETCS_REPL_SHELL

// ---------------------------------------------------------------------------
// Navigation. ETCS_LOADER only -- it reads the live entity graph and has no
// terminal dependency of its own.
// ---------------------------------------------------------------------------
#ifdef ETCS_LOADER

// Names published by the ROOT SCRIPT, filtered to one module.
//
// This replaces what used to read PersistentNames, and shows strictly less,
// on purpose. PersistentNames accumulated every name every script had ever
// bound, at any depth, for the life of the process -- so this display was a
// history of everything anyone had ever called anything. GlobalNames holds
// only the names the root script itself introduced, which is the only set a
// navigator can meaningfully offer: they are exactly the names any script in
// the tree can reach by `attach` or `ensure`.
//
// Liveness is verified here rather than trusted -- nothing prunes an entry
// when its entity dies, so a name whose target is gone is skipped instead of
// shown as reachable.
inline std::vector<std::pair<std::string, ETCS::NameBinding>>
repl_live_globals_for_module(const std::string& mod_name)
{
    std::vector<std::pair<std::string, ETCS::NameBinding>> out;
    auto& ridMap = ETCS::EventNode::getInstance().ridMap;
    for (auto& [name, b] : ETCS::GlobalNames::getInstance().snapshot())
    {
        if (b.module != mod_name) continue;
        ETCS::Buffer key;
        key.writeString((b.module + ":" + b.tag).c_str());
        auto it = ridMap.find(key);
        if (it != ridMap.end() && it->second.invoke_contains(b.rid))
            out.emplace_back(name, b);
        else
            ETCS::GlobalNames::getInstance().forget(name);   // dead: retract it
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& c) { return a.first < c.first; });
    return out;
}

// An ExecutionContext for one navigator dispatch.
//
// is_root is FALSE, always. The navigator must never publish into
// GlobalNames: those are the root script's names, and a name invented here to
// address a selected entity is a fixture of this menu, not part of any
// script's closure. Defaulting to true (which ExecutionContext does, for
// scripts) would leak `self` into every tree launched afterward.
//
// The selected entity is bound under a fixed name so the Command values below
// have a receiver to carry -- every command now names one, and the navigator
// is not exempt just because a menu makes the target obvious.
inline ETCS::ExecutionContext repl_nav_context(const std::string& mod_name,
                                               const std::string& tag_name,
                                               ETCS::RID rid,
                                               ETCS::Root& nav_root,
                                               ETCS::SignalContext& sig)
{
    ETCS::ExecutionContext ctx;
    ctx.sig         = &sig;
    ctx.root_entity = &nav_root;
    ctx.is_root     = false;
    ctx.bind("self", ETCS::NameBinding{rid, mod_name, tag_name});
    return ctx;
}

inline void repl_shell_print_dir(std::vector<std::string>& mods)
{
    mods.clear();
    ETCS_SHELL("ShellREPL", "\n" << COLOR_DIR << "[ " << fs::current_path().string()
             << " ]" << COLOR_RESET);
    for (const auto& entry : fs::directory_iterator("."))
    {
        if (entry.is_directory())
        {
            ETCS_SHELL("ShellREPL", COLOR_DIR << "  [DIR] "
                     << entry.path().filename().string() << COLOR_RESET);
        }
        else if (repl_is_module(entry))
        {
            ETCS_SHELL("ShellREPL", COLOR_LIB << "  [" << mods.size() << "] "
                     << entry.path().stem().string() << COLOR_RESET);
            mods.push_back(entry.path().stem().string());
        }
        else if (entry.path().extension() == ".etcs")
        {
            ETCS_SHELL("ShellREPL", COLOR_EXEC << "  [ETCS] "
                     << entry.path().filename().string() << COLOR_RESET);
        }
        else
        {
            ETCS_SHELL("ShellREPL", "        " << entry.path().filename().string());
        }
    }
}

// Every module loaded anywhere in the process, whether or not its .so is in
// the current directory -- a module started by a detached script was
// otherwise reachable only by typing its name blind. Root level only: a
// module you can jump to belongs above any single entity.
//
// Reads LoaderStream::module_registry directly, hence the ETCS_LOADER gate.
// Skips null values: vacancy is a null VALUE there, never row erasure, and a
// vacant entry re-bootstraps exactly like a never-seen name would.
//
// Appends onto the same all_mods vector repl_shell_print_dir filled, so
// numeric selection keeps working unchanged.
inline void repl_shell_print_live_modules(std::vector<std::string>& all_mods)
{
    std::unordered_set<std::string> already(all_mods.begin(), all_mods.end());
    std::vector<std::string> live_only;

    auto& registry = ETCS::EventNode::getInstance().stream.module_registry;
    for (const auto& [name, mod_ptr] : registry)
    {
        if (!mod_ptr) continue;
        if (already.count(name)) continue;
        live_only.push_back(name);
    }
    std::sort(live_only.begin(), live_only.end());
    if (live_only.empty()) return;

    ETCS_SHELL("ShellREPL", COLOR_LIB
             << "  --- Live modules (loaded, not in this directory) ---" << COLOR_RESET);
    for (size_t i = 0; i < live_only.size(); ++i)
    {
        ETCS_SHELL("ShellREPL", COLOR_LIB << "  [" << all_mods.size() + i << "] "
                 << live_only[i] << COLOR_RESET);
    }
    for (auto& m : live_only) all_mods.push_back(std::move(m));
}

// ── Action loop ─────────────────────────────────────────────────────────────
// nav_root is the navigation scope's own Root (repl_shell_loop_with), threaded
// down purely as a bootstrap anchor for whatever a dispatched action needs to
// resolve -- never as "the module e belongs to".
//
// `e` is valid only until the next blocking read. This loop waits on input, so
// another thread (a detached script deleting this entity) can destroy it
// mid-wait. resolve_self() is the single re-resolution point; nothing touches
// `e` past a read without it. That was true of a human at a keyboard and is no
// less true of a remote session -- the wait is what matters, not what is being
// waited on.
inline void repl_shell_action_loop(ETCS::Entity* e, ETCS::Root& nav_root,
                                   ETCS::SignalContext& sig, ReplLineSource& in)
{
    ETCS::ExecSource src{"(interactive)", 0};

    // Derived from the entity, never passed in -- getSourceModule/Tag are
    // already the authoritative identity (setModuleSource at attach time).
    const std::string mod_name = e->getSourceModule().toString();
    const std::string tag_name = e->getSourceTag().toString();

    // e->module_.catalog(), not nav_root's: every live entity carries its own
    // attached module_ token from construction.
    auto& catalog = e->module_.catalog();
    auto cat_it = catalog.find(tag_name);
    if (cat_it == catalog.end())
    {
        ETCS_SHELL("ShellREPL", COLOR_WARN << "Type '" << tag_name
            << "' has no catalog entry in " << mod_name << COLOR_RESET);
        return;
    }
    const ETCS::ModuleBundle& bundle = cat_it->second;

    std::vector<std::pair<ETCS::Buffer, ETCS::WorkBundle>> action_list;
    for (const auto& pair : bundle.actions)
        action_list.emplace_back(pair.first, pair.second);

    // RID is the stable identity; the pointer is not.
    const ETCS::RID target_rid = e->getRID();

    // Replaces the old still_alive() bool -- returning the pointer means a
    // caller can't keep using the stale one after a successful check.
    auto resolve_self = [&]() -> ETCS::Entity*
    {
        ETCS::Buffer key;
        key.writeString((mod_name + ":" + tag_name).c_str());
        auto& ridMap = ETCS::EventNode::getInstance().ridMap;
        auto it = ridMap.find(key);
        if (it == ridMap.end()) return nullptr;
        return it->second.invoke_get(target_rid);
    };

    auto resolve_other = [](const std::string& m, const std::string& t,
                            ETCS::RID rid) -> ETCS::Entity*
    {
        ETCS::Buffer key;
        key.writeString((m + ":" + t).c_str());
        auto& ridMap = ETCS::EventNode::getInstance().ridMap;
        auto it = ridMap.find(key);
        if (it == ridMap.end()) return nullptr;
        return it->second.invoke_get(rid);
    };

    while (true)
    {
        ETCS_SHELL("ShellREPL", "\n--- Actions for " << COLOR_DIR << tag_name
            << COLOR_RESET << " [RID:" << COLOR_RID << target_rid << COLOR_RESET << "] ---");
        for (size_t i = 0; i < action_list.size(); ++i)
        {
            const auto& [action_name, work] = action_list[i];
            ETCS_SHELL("ShellREPL", COLOR_ACT << "  [" << i << "] "
                << tag_name << "." << action_name.toString()
                << (work.isStream ? "  [stream]" : "") << COLOR_RESET);
        }

        // Identity, not pointer -- `parent` goes stale across the read below
        // exactly like `e` does, and the `up` branch dereferences it after.
        std::string  parent_mod, parent_tag;
        ETCS::RID    parent_rid = 0;
        if (ETCS::Entity* parent = e->getParent())
        {
            parent_mod = parent->getSourceModule().toString();
            parent_tag = parent->getSourceTag().toString();
            parent_rid = parent->getRID();
            ETCS_SHELL("ShellREPL", COLOR_DIR << "  [up] " << COLOR_RESET
                << parent_mod << "::" << parent_tag
                << " [RID:" << COLOR_RID << parent_rid << COLOR_RESET << "]");
        }

        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> children;
        e->getTypedChildren(children);
        if (!children.empty())
        {
            ETCS_SHELL("ShellREPL", COLOR_DIR << "  --- Children ---" << COLOR_RESET);
            for (size_t i = 0; i < children.size(); ++i)
            {
                ETCS_SHELL("ShellREPL", COLOR_DIR << "  [c" << i << "] " << COLOR_RESET
                    << children[i].first.toString() << " [RID:" << COLOR_RID
                    << children[i].second << COLOR_RESET << "]");
            }
        }

        // Live in-flight work functions, in creation order -- the only
        // explicit record of the causal sequence that produced them (see
        // Scope, Bundles.h). Two ways to act on what's listed:
        //
        //   s<n>                 -- flat, positional against THIS listing.
        //                           Addresses a display nothing else prints,
        //                           so it is a navigator affordance. Fast for
        //                           a handful.
        //   kill <label> [index] -- by name, without reading the list, which
        //                           is what matters once an entity carries
        //                           enough concurrent work that scanning stops
        //                           being how you find anything.
        //
        // Both build a CmdKill and go through execute_command, so the
        // navigator and scripts cannot drift in behavior. Neither is script
        // syntax -- a script writes `<name>.kill(Listen)`, because a script
        // has a name for its entity and this menu has a selection.
        std::vector<ETCS::Scope::View> scopes;
        e->collectScopes(scopes);
        if (!scopes.empty())
        {
            ETCS_SHELL("ShellREPL", COLOR_WARN << "  --- Active work ---" << COLOR_RESET);
            for (size_t i = 0; i < scopes.size(); ++i)
            {
                ETCS_SHELL("ShellREPL", COLOR_WARN << "  [s" << i << "] " << COLOR_RESET
                    << scopes[i].label << " " << COLOR_RID << scopes[i].index << COLOR_RESET
                    << (scopes[i].interrupted ? "  (stopping)" : ""));
            }
            ETCS_SHELL("ShellREPL",
                "  [s<n>] Interrupt by position   [kill <label> [index]] Interrupt directly");
        }

        std::string a_in;
        if (!in(tag_name + " Act> ", a_in)) break;
        if (a_in == "back")                   { break; }
        if (a_in == "exit" || a_in == "quit") { return; }
        if (sig.isInterrupted() || sig.isTerminated()) break;

        // Re-resolve before touching `e` again.
        e = resolve_self();
        if (!e)
        {
            ETCS_SHELL("ShellREPL", COLOR_WARN << "Entity RID:" << target_rid
                << " was destroyed while awaiting input -- returning to the "
                   "instance list." << COLOR_RESET);
            return;
        }

        if (a_in == "up")
        {
            ETCS::Entity* parent = parent_rid
                ? resolve_other(parent_mod, parent_tag, parent_rid) : nullptr;
            if (parent)
            {
                repl_shell_action_loop(parent, nav_root, sig, in);
            }
            else if (parent_rid)
            {
                ETCS_SHELL("ShellREPL", COLOR_WARN << "Parent RID:" << parent_rid
                    << " is no longer alive." << COLOR_RESET);
            }
            else
            {
                ETCS_SHELL("ShellREPL", COLOR_WARN << "This entity has no parent." << COLOR_RESET);
            }
            e = resolve_self();
            if (!e)
            {
                ETCS_SHELL("ShellREPL", COLOR_WARN << "Entity RID:" << target_rid
                    << " no longer exists -- returning to the instance list." << COLOR_RESET);
                return;
            }
            continue;
        }

        if (a_in.size() > 1 && a_in[0] == 'c' && std::isdigit((unsigned char)a_in[1]))
        {
            try {
                size_t idx = std::stoul(a_in.substr(1));
                if (idx < children.size())
                {
                    // getTypedChild is a RID lookup -- re-resolved already.
                    ETCS::Entity* child = e->getTypedChild(children[idx].first,
                                                           children[idx].second);
                    if (child)
                    {
                        repl_shell_action_loop(child, nav_root, sig, in);
                    }
                    else
                    {
                        ETCS_SHELL("ShellREPL", COLOR_WARN << "Child no longer alive." << COLOR_RESET);
                    }
                }
                else
                {
                    ETCS_SHELL("ShellREPL", COLOR_WARN << "Invalid child index." << COLOR_RESET);
                }
            } catch (...) {
                ETCS_SHELL("ShellREPL", COLOR_WARN << "Invalid child selector." << COLOR_RESET);
            }
            e = resolve_self();
            if (!e)
            {
                ETCS_SHELL("ShellREPL", COLOR_WARN << "Entity RID:" << target_rid
                    << " no longer exists -- returning to the instance list." << COLOR_RESET);
                return;
            }
            continue;
        }

        // s<n> -- translated back to (label, index) from the SAME snapshot
        // that was displayed, so what gets interrupted is what was shown on
        // that line, not whatever now occupies that position. `scopes` was
        // collected before the blocking read, so an entry may have finished
        // since; execute_command reports that rather than failing silently.
        //
        // Unambiguous by ABI, not by convention: action names are TitleCase as
        // a structural rule of this runtime (enforced at the marketplace
        // boundary the same way the type structure is), so no action can ever
        // be spelled s0 -- exactly why back/up/kill are safe too. No
        // disambiguation check here on purpose; adding one would imply the
        // runtime permits something it actually forbids.
        if (a_in.size() > 1 && a_in[0] == 's' && std::isdigit((unsigned char)a_in[1]))
        {
            try {
                size_t idx = std::stoul(a_in.substr(1));
                if (idx < scopes.size())
                {
                    ETCS::ExecutionContext kctx =
                        repl_nav_context(mod_name, tag_name, target_rid, nav_root, sig);
                    ETCS::CmdKill kcmd;
                    kcmd.receiver  = "self";
                    kcmd.label     = scopes[idx].label;
                    kcmd.index     = scopes[idx].index;
                    kcmd.has_index = true;
                    repl_out() << COLOR_EXEC;
                    ETCS::execute_command(ETCS::Command{kcmd}, kctx, src);
                    repl_out() << COLOR_RESET;
                }
                else
                {
                    ETCS_SHELL("ShellREPL", COLOR_WARN << "Invalid scope index." << COLOR_RESET);
                }
            } catch (...) {
                ETCS_SHELL("ShellREPL", COLOR_WARN << "Invalid scope selector." << COLOR_RESET);
            }
            continue;
        }

        // kill <label> [index] -- built here rather than routed through
        // parse_line. The navigator's input is not the script grammar and no
        // longer pretends to be: `kill Listen` is a menu command against the
        // current selection, while a script writes `<name>.kill(Listen)`
        // because a script has a name and this menu has a selection. What is
        // shared is execute_command -- the behavior -- not the syntax.
        if (a_in.rfind("kill", 0) == 0
            && (a_in.size() == 4 || a_in[4] == ' ' || a_in[4] == '\t'))
        {
            std::string rest = (a_in.size() > 4) ? a_in.substr(5) : "";
            size_t rb = rest.find_first_not_of(" \t");
            rest = (rb == std::string::npos) ? "" : rest.substr(rb);
            if (rest.empty())
            {
                repl_err() << COLOR_WARN
                           << "kill: expected a work-function label, e.g. 'kill Listen'"
                           << COLOR_RESET << "\n";
                continue;
            }
            ETCS::CmdKill kcmd;
            kcmd.receiver = "self";
            size_t sp2 = rest.find_first_of(" \t");
            kcmd.label = (sp2 == std::string::npos) ? rest : rest.substr(0, sp2);
            if (sp2 != std::string::npos)
            {
                std::string idx_str = rest.substr(sp2 + 1);
                size_t ib = idx_str.find_first_not_of(" \t");
                idx_str = (ib == std::string::npos) ? "" : idx_str.substr(ib);
                if (!idx_str.empty())
                {
                    try {
                        size_t end;
                        kcmd.index = static_cast<size_t>(std::stoull(idx_str, &end));
                        if (end != idx_str.size()) throw std::invalid_argument("trailing");
                        kcmd.has_index = true;
                    } catch (...) {
                        repl_err() << COLOR_WARN << "kill: invalid index '" << idx_str
                                   << "'" << COLOR_RESET << "\n";
                        continue;
                    }
                }
            }
            ETCS::ExecutionContext kctx =
                repl_nav_context(mod_name, tag_name, target_rid, nav_root, sig);
            repl_out() << COLOR_EXEC;
            ETCS::execute_command(ETCS::Command{kcmd}, kctx, src);
            repl_out() << COLOR_RESET;
            continue;
        }

        // An action: a name or an index, then everything after the first
        // space as the payload. Unbracketed, deliberately -- this is the
        // browse surface, where you are picking an action off a menu you are
        // looking at, not writing a line someone reads back later. The
        // brackets exist in the script grammar to remove an ambiguity about
        // where a payload starts; here there is no selector to confuse it
        // with, because the selection is the menu.
        std::string action_str = a_in;
        std::string payload_str;
        size_t sp = a_in.find(' ');
        if (sp != std::string::npos)
        {
            action_str  = a_in.substr(0, sp);
            payload_str = a_in.substr(sp + 1);
        }
        if (!action_str.empty() && std::isdigit((unsigned char)action_str[0]))
        {
            try {
                size_t idx = std::stoul(action_str);
                if (idx < action_list.size())
                    action_str = action_list[idx].first.toString();
            } catch (...) {}
        }

        ETCS::ExecutionContext ctx =
            repl_nav_context(mod_name, tag_name, target_rid, nav_root, sig);

        ETCS::CmdAction cmd;
        cmd.receiver = "self";
        cmd.action   = action_str;
        cmd.payload  = payload_str;

#ifdef ETCS_REPL_SHELL
        repl_clear_signal_flags();
#endif
        repl_out() << COLOR_EXEC;
        ETCS::ExecuteResult result = ETCS::execute_command(ETCS::Command{cmd}, ctx, src);
        repl_out() << COLOR_RESET;
        if (result.status == ETCS::ExecuteStatus::Fatal) return;

#ifdef ETCS_REPL_SHELL
        if (repl_sigint_raised())
        {
            ETCS_SHELL("ShellREPL", COLOR_WARN << "\n[SIGNAL] Action interrupted." << COLOR_RESET);
            g_sig_int.store(0, std::memory_order_release);
        }
#endif
        e = resolve_self();
        if (!e)
        {
            ETCS_SHELL("ShellREPL", COLOR_WARN << "Entity RID:" << target_rid
                << " no longer exists (destroyed by the action just run) "
                   "-- returning to the instance list." << COLOR_RESET);
            return;
        }
    }
}

// ── Instance loop ───────────────────────────────────────────────────────────
inline void repl_shell_instance_loop(const std::string& mod_name, const std::string& tag_name,
                                     ETCS::Root& nav_root, ETCS::SignalContext& sig,
                                     ReplLineSource& in)
{
    ETCS::ExecSource src{"(interactive)", 0};

    while (true)
    {
        if (sig.isInterrupted() || sig.isTerminated()) break;

        ETCS::Buffer key;
        key.writeString((mod_name + ":" + tag_name).c_str());
        auto& ridMap = ETCS::EventNode::getInstance().ridMap;
        auto it = ridMap.find(key);
        const ETCS::RIDListHandle* handle = (it != ridMap.end()) ? &it->second : nullptr;
        if (!handle)
        {
            ETCS_SHELL("ShellREPL", COLOR_WARN << " Tag is invalid!" << COLOR_RESET);
            return;
        }

        std::vector<ETCS::RID> live_rids;
        handle->invoke_collect_rids(live_rids);

        ETCS_SHELL("ShellREPL", "\n--- Live instances of " << COLOR_LIB << tag_name
            << COLOR_RESET << " in " << COLOR_DIR << mod_name << COLOR_RESET
            << " [" << COLOR_RID << live_rids.size() << COLOR_RESET << " active] ---");

        if (live_rids.empty())
        {
            ETCS_SHELL("ShellREPL",
                COLOR_WARN << "  (none — spawn one from the tag menu first)" << COLOR_RESET);
        }
        else
        {
            // Once, not per-instance.
            auto named_here = repl_live_globals_for_module(mod_name);
            for (size_t i = 0; i < live_rids.size(); ++i)
            {
                std::string alias;
                for (auto& [name, b] : named_here)
                    if (b.tag == tag_name && b.rid == live_rids[i])
                        { alias = " (" + name + ")"; break; }
                ETCS_SHELL("ShellREPL", COLOR_RID << "  [" << i
                    << "] RID:" << live_rids[i] << COLOR_RESET << alias);
            }
        }
        ETCS_SHELL("ShellREPL",
            "  [n] Select   [spawn <name>] Create and name   [back] Return");

        std::string i_in;
        if (!in(tag_name + " Inst> ", i_in)) break;
        if (i_in == "back")                   { break; }
        if (i_in == "exit" || i_in == "quit") { return; }

        // spawn <name> -- the name is REQUIRED, and goes into GlobalNames.
        //
        // An unnamed entity can only be passed by reading its RID out of a log
        // and retyping it, which is what "no RIDs in the script itself" exists
        // to prevent; the prompt does not get to be where that leaks.
        //
        // GlobalNames, not a table of the navigator's own -- there is no third
        // scope. When no root script is running the prompt is what populates
        // the runtime, so the prompt publishes its names, and a script reaches
        // them by attach/ensure/requires with no binding threaded down.
        if (i_in == "spawn" || i_in.rfind("spawn ", 0) == 0)
        {
            std::string sname = (i_in.size() > 5) ? i_in.substr(6) : "";
            size_t nb = sname.find_first_not_of(" \t");
            size_t ne = sname.find_last_not_of(" \t");
            sname = (nb == std::string::npos) ? "" : sname.substr(nb, ne - nb + 1);

            if (sname.empty())
            {
                repl_err() << COLOR_WARN
                           << "spawn: expected a name -- 'spawn <name>'. The name is how "
                              "anything else reaches it: @name in a payload, or a script's "
                              "own attach/ensure/requires."
                           << COLOR_RESET << "\n";
                continue;
            }
            // Same rules a script's names obey. `self` is reserved because
            // every navigator dispatch binds the selection under it.
            bool valid = (sname != "root" && sname != "self");
            for (char c : sname)
                if (!std::isalnum((unsigned char)c) && c != '_') { valid = false; break; }
            if (!valid)
            {
                repl_err() << COLOR_WARN << "spawn: '" << sname
                           << "' is not a usable name." << COLOR_RESET << "\n";
                continue;
            }
            // Same refusal a script's `spawn` gets. No `attach` to suggest
            // here -- the equivalent is selecting the existing instance.
            // live_global, not find: a global whose entity was deleted is
            // retracted on this first miss, so the name frees up instead of
            // staying spoken-for by something that no longer exists.
            if (auto prior = ETCS::live_global(sname))
            {
                repl_err() << COLOR_WARN << "spawn: clobbering global '" << sname
                           << "' (" << prior->module << "::" << prior->tag
                           << " RID:" << prior->rid << "). Select that instance instead, "
                              "or pick another name."
                           << COLOR_RESET << "\n";
                continue;
            }

            ETCS::ExecutionContext ctx;
            ctx.sig     = &sig;
            ctx.is_root = false;
            // Module is already anchored, so loadImpl's vacant branch never
            // runs here -- this satisfies spawn_entity's non-null guard and
            // gives resolve_module/verify_tag something to work against.
            ctx.root_entity = &nav_root;
            ETCS::Entity* e = ETCS::spawn_entity(mod_name, tag_name, ctx, src);
            if (e)
            {
                ETCS::GlobalNames::getInstance().record(
                    sname, ETCS::NameBinding{e->getRID(), mod_name, tag_name});
                ETCS_SHELL("ShellREPL", COLOR_LIB << "spawned '" << sname << "' -> RID:"
                         << e->getRID() << COLOR_RESET
                         << " -- reachable as a global by any script from here.");
                repl_shell_action_loop(e, nav_root, sig, in);
            }
            continue;
        }

        if (!i_in.empty() && std::isdigit((unsigned char)i_in[0]))
        {
            try {
                size_t idx = std::stoul(i_in);
                if (idx < live_rids.size())
                {
                    ETCS::Entity* e = handle->invoke_get(live_rids[idx]);
                    if (e)
                    {
                        repl_shell_action_loop(e, nav_root, sig, in);
                    }
                    else
                    {
                        ETCS_SHELL("ShellREPL", COLOR_WARN << "Entity dead." << COLOR_RESET);
                    }
                }
            } catch (...) {
                ETCS_SHELL("ShellREPL", COLOR_WARN << "Invalid index." << COLOR_RESET);
            }
            continue;
        }
    }
}

// ── Tag loop ────────────────────────────────────────────────────────────────
// nav_root's module_ is already bound by repl_shell_loop_with before this runs.
inline void repl_shell_tag_loop(const std::string& mod_name, ETCS::Root& nav_root,
                                ETCS::SignalContext& sig, ReplLineSource& in)
{
    bool detach_module = false;
    const std::vector<ETCS::Buffer>& tags = nav_root.module_.getTags();

    while (true)
    {
        if (sig.isInterrupted() || sig.isTerminated()) break;

        ETCS_SHELL("ShellREPL", "\n--- Tags in " << COLOR_LIB << mod_name << COLOR_RESET << " ---");
        for (size_t i = 0; i < tags.size(); ++i)
        {
            ETCS::Buffer key;
            key.writeString((mod_name + ":" + tags[i].toString()).c_str());
            auto& ridMap = ETCS::EventNode::getInstance().ridMap;
            auto it = ridMap.find(key);
            size_t live_count = (it != ridMap.end()) ? it->second.invoke_count() : 0;
            ETCS_SHELL("ShellREPL", COLOR_LIB << "  [" << i << "] " << tags[i].toString()
                << COLOR_RESET << "  (" << COLOR_RID << live_count << " live" << COLOR_RESET << ")");
        }

        // The root script's own names -- the globals every script in its tree
        // can reach by attach or ensure. Empty when no root script is running,
        // which is correct rather than a gap: with nothing composing, there is
        // no shared vocabulary to show.
        {
            auto alive_named = repl_live_globals_for_module(mod_name);
            if (!alive_named.empty())
            {
                ETCS_SHELL("ShellREPL", COLOR_DIR
                    << "  --- Root script names (reachable by attach/ensure) ---"
                    << COLOR_RESET);
                for (auto& [name, b] : alive_named)
                {
                    ETCS_SHELL("ShellREPL", COLOR_RID << "  " << name << COLOR_RESET
                        << " -> " << b.tag << " RID:" << COLOR_RID << b.rid << COLOR_RESET);
                }
            }
        }

        ETCS_SHELL("ShellREPL", "  [back] Return   [detach] Detach   [exit] Quit");

        std::string t_in;
        if (!in(mod_name + " Tag> ", t_in)) break;
        if (t_in.empty()) continue;
        if (t_in == "back")   { break; }
        if (t_in == "detach") { detach_module = true; break; }
        if (t_in == "exit" || t_in == "quit") { return; }

        std::string tag_name = t_in;
        try {
            if (!t_in.empty() && std::isdigit((unsigned char)t_in[0]))
            {
                size_t idx = std::stoul(t_in);
                if (idx < tags.size()) tag_name = tags[idx].toString();
            }
        } catch (...) {}

        repl_shell_instance_loop(mod_name, tag_name, nav_root, sig, in);
    }

    if (detach_module)
    {
        ETCS_SHELL("ShellREPL",
            "Detaching module: " << mod_name << " (leaving live entities running).");
    }
}

#endif // ETCS_LOADER

#if defined(ETCS_REPL_SHELL) && defined(__linux__)
// Defined below, after repl_shell_loop -- the root loop's `attach` branch
// reaches it, and the attach relay in turn borrows repl_shell_get_input, so
// one of the two has to be forward-declared. This one, since the root loop is
// the shared entry point and belongs earlier.
inline void repl_shell_attach_loop(const std::string& path, ETCS::SignalContext& sig);
#endif

// ── Root loop ───────────────────────────────────────────────────────────────
// A FRESH nav_root per module navigation, popped when repl_shell_tag_loop
// returns. That is what keeps attachModule's "one module per entity/Root"
// guard from biting: nav_root is never reused across two resolutions, so
// there's no stale binding to collide with. If nothing was spawned, its
// destruction unloads the module (sibling-Root search first, see
// changeModuleImpl); if something was, ownership already transferred to that
// entity and the destruction is a no-op.
//
// Takes its input source rather than reading stdin, so the same loop serves a
// local terminal and a socket session. Nothing below knows which it is.
//
// The directory listing and `cd` are the SERVER's filesystem when this runs
// under a session, which is correct: that is where its scripts and modules
// live, and a remote operator wanting to run one wants to see them.
//
// `jobs` and `signal` live HERE and nowhere else now. They were briefly also
// script verbs, which was always wrong: a script launches work with
// detach/run, it does not administer it afterward. Administration is what a
// prompt is for.
inline void repl_shell_loop_with(ETCS::SignalContext& sig, ReplLineSource& in)
{
    while (!(sig.isInterrupted() || sig.isTerminated()))
    {
        std::vector<std::string> available_mods;
#ifdef ETCS_LOADER
        repl_shell_print_dir(available_mods);
        repl_shell_print_live_modules(available_mods);
#endif
        ETCS_SHELL("ShellREPL", "--------------------------------------------------------");

        std::string mod_input;
        if (!in("Root> ", mod_input)) break;
        if (mod_input == "exit" || mod_input == "quit") break;
        if (mod_input.empty()) continue;

        if (mod_input.substr(0, 3) == "cd ")
        {
            try { fs::current_path(mod_input.substr(3)); }
            catch (const std::exception& e)
                { repl_err() << COLOR_WARN << "CD Error: " << e.what() << COLOR_RESET << "\n"; }
            continue;
        }

        /*
         * WHERE THE LOG GOES, changed while it is running.
         *
         * It matters most exactly here: a prompt sharing stdout with a frame
         * loop's logging is a prompt you cannot read, and the answer used to
         * be a rebuild. Each module keeps its own destination (ETCS::
         * log_to_file, Log.h) and writes to logs/<ModuleName>.log, so this
         * visits every loaded one rather than flipping a single global.
         */
        if (mod_input == "log" || mod_input.rfind("log ", 0) == 0)
        {
            std::string arg = (mod_input.size() > 4) ? mod_input.substr(4) : "";
            size_t b0 = arg.find_first_not_of(" \t");
            size_t b1 = arg.find_last_not_of(" \t");
            arg = (b0 == std::string::npos) ? "" : arg.substr(b0, b1 - b0 + 1);

            if (arg == "file" || arg == "term" || arg == "terminal")
            {
                const bool to_file = (arg == "file");
                ETCS::set_log_destination(to_file);
                ETCS_SHELL("ShellREPL", COLOR_LIB << "log -> "
                         << (to_file ? "logs/<Module>.log (one file per provider)"
                                     : "this terminal")
                         << COLOR_RESET);
            }
            else if (arg.empty() || arg == "status")
            {
                ETCS_SHELL("ShellREPL", COLOR_DIR << "log destination: "
                         << (ETCS::log_destination_is_file()
                             ? "logs/<Module>.log" : "terminal")
                         << COLOR_RESET << "   (log file | log term)");
            }
            else
            {
                repl_err() << COLOR_WARN << "log: expected 'file', 'term', or nothing"
                           << COLOR_RESET << "\n";
            }
            continue;
        }

        if (mod_input == "jobs")
        {
            auto jobs = ETCS::DetachedRegistry::getInstance().list();
            if (jobs.empty())
            {
                ETCS_SHELL("ShellREPL", COLOR_WARN << "  (no detached scripts running)" << COLOR_RESET);
            }
            else
            {
                ETCS_SHELL("ShellREPL", COLOR_DIR << "--- Detached scripts ---" << COLOR_RESET);
                for (const auto& [id, script] : jobs)
                {
                    ETCS_SHELL("ShellREPL", COLOR_RID << "  [" << id << "] " << COLOR_RESET << script);
                }
            }
            continue;
        }

        if (mod_input.rfind("signal ", 0) == 0)
        {
            std::string rest = mod_input.substr(7);
            size_t sp = rest.find_first_of(" \t");
            std::string id_str = (sp == std::string::npos) ? rest : rest.substr(0, sp);
            std::string mode   = (sp == std::string::npos) ? "" : rest.substr(sp + 1);
            uint64_t id = 0;
            bool ok = true;
            try {
                size_t end;
                id = std::stoull(id_str, &end);
                if (end != id_str.size()) ok = false;
            } catch (...) { ok = false; }
            if (!ok)
            {
                repl_err() << COLOR_WARN << "signal: invalid id '" << id_str << "'"
                           << COLOR_RESET << "\n";
                continue;
            }
            bool term  = (mode != "interrupt" && mode != "int");
            bool found = term ? ETCS::DetachedRegistry::getInstance().terminate(id)
                              : ETCS::DetachedRegistry::getInstance().interrupt(id);
            if (found)
            {
                ETCS_SHELL("ShellREPL", COLOR_LIB << (term ? "Terminating" : "Interrupting")
                         << " job [" << id << "]..." << COLOR_RESET);
            }
            else
            {
                repl_err() << COLOR_WARN << "signal: no job with id " << id
                           << COLOR_RESET << "\n";
            }
            continue;
        }

#if defined(ETCS_REPL_SHELL) && defined(__linux__)
        // Terminal-only: the attach relay borrows this process's own line
        // editor, which a session driving us does not have to lend.
        if (mod_input.rfind("attach ", 0) == 0)
        {
            std::string apath = mod_input.substr(7);
            size_t as = apath.find_first_not_of(" \t");
            size_t ae = apath.find_last_not_of(" \t");
            if (as == std::string::npos)
            {
                repl_err() << COLOR_WARN << "attach: expected a socket path"
                           << COLOR_RESET << "\n";
                continue;
            }
            repl_shell_attach_loop(apath.substr(as, ae - as + 1), sig);
            // Clear so a Ctrl+C aimed at the remote session doesn't also exit
            // this REPL -- same reason the script path clears it.
            g_sig_int.store(0, std::memory_order_release);
            continue;
        }
#endif

        // Separate the module/script target from any injection arguments.
        std::istringstream iss(mod_input);
        std::string target_str;
        iss >> target_str;

        std::string mod_name = target_str;
        try {
            if (!target_str.empty() && std::isdigit((unsigned char)target_str[0]))
            {
                size_t idx = std::stoul(target_str);
                if (idx < available_mods.size()) mod_name = available_mods[idx];
            }
        } catch (...) {}

        // ── Script execution mode ──────────────────────────────────────────
        //
        // Goes through run_root_script, which PREFLIGHTS the whole tree --
        // every detach and run target, recursively -- and refuses to start
        // anything if the graph does not resolve. That is the one place the
        // check belongs: preflight is a property of an invocation, and this is
        // where invocations begin.
        if (mod_name.length() >= 5 && mod_name.substr(mod_name.length() - 5) == ".etcs")
        {
#ifdef ETCS_LOADER
            // Fresh Root scoped to this script execution.
            ETCS::Root script_root(sig);
            ETCS::ExecutionContext script_ctx;
            script_ctx.sig         = &sig;
            script_ctx.root_entity = &script_root;
            script_ctx.is_root     = true;   // its names become the globals

            std::string arg;
            bool args_valid = true;
            while (iss >> arg)
            {
                if (sig.isInterrupted() || sig.isTerminated()) break;
                auto eq = arg.find('=');
                if (eq == std::string::npos || eq == 0 || eq == arg.size() - 1)
                {
                    repl_err() << COLOR_WARN << "ShellREPL: invalid injection argument '"
                               << arg << "' -- expected name=globalname or name=RID" << COLOR_RESET << "\n";
                    args_valid = false;
                    break;
                }
                std::string name    = arg.substr(0, eq);
                std::string rid_str = arg.substr(eq + 1);

                bool valid_name = !name.empty() && name != "root";
                for (char c : name)
                    if (!std::isalnum((unsigned char)c) && c != '_') { valid_name = false; break; }
                if (!valid_name)
                {
                    repl_err() << COLOR_WARN << "ShellREPL: invalid name '" << name
                               << "' in injection argument '" << arg << "'"
                               << COLOR_RESET << "\n";
                    args_valid = false;
                    break;
                }
                // A global NAME first, a raw RID only as fallback --
                // `script.etcs game=node` is what a detach line already looks
                // like, so the prompt is not where that becomes a number.
                //
                // Needed ONLY to RENAME: if the script says `requires node`, a
                // global `node` satisfies it with no argument at all.
                ETCS::NameBinding nb{};
                if (auto g = ETCS::live_global(rid_str))
                {
                    nb = *g;
                }
                else
                {
                    try {
                        size_t end;
                        unsigned long long rid_v = std::stoull(rid_str, &end);
                        if (end != rid_str.size()) throw std::invalid_argument("trailing");
                        nb.rid = static_cast<ETCS::RID>(rid_v);
                    } catch (...) {
                        repl_err() << COLOR_WARN << "ShellREPL: '" << rid_str
                                   << "' (in '" << arg << "') is neither a global name "
                                      "nor a RID." << COLOR_RESET << "\n";
                        args_valid = false;
                        break;
                    }
                }

                // Resolved HERE, not deferred. A binding carries its
                // Module::Tag now (action lines no longer state one), and a
                // bare RID is the one place that pair is not already known --
                // so it is recovered from the entity itself, which also
                // settles liveness at capture time.
                ETCS::Entity* e = ETCS::resolve_bound_entity(nb);
                if (!e)
                {
                    repl_err() << COLOR_WARN << "ShellREPL: '" << rid_str
                               << "' (for '" << name << "') does not resolve to a live "
                                  "entity." << COLOR_RESET << "\n";
                    args_valid = false;
                    break;
                }
                if (nb.tag.empty())
                {
                    nb.module = e->getSourceModule().toString();
                    nb.tag    = e->getSourceTag().toString();
                }
                script_ctx.bind(name, nb);
                ETCS_SHELL("ShellREPL", "Injected: " << name << " -> RID:" << nb.rid
                         << " (" << nb.module << "::" << nb.tag << ")");
            }
            if (!args_valid) continue;

            ETCS::run_root_script(mod_name, script_ctx);

            // Clear so a Ctrl+C aimed at the script doesn't also exit the REPL.
            g_sig_int.store(0, std::memory_order_release);
#else
            repl_err() << COLOR_WARN
                       << "ShellREPL: script execution needs the loader." << COLOR_RESET << "\n";
#endif
            continue;
        }

#ifdef ETCS_LOADER
        try {
            ETCS::Root nav_root(sig);
            if (!ETCS::ResolveEvent{mod_name.c_str(), &nav_root}())
            {
                ETCS_SHELL("ShellREPL", COLOR_WARN << "Failed to load module: "
                    << mod_name << COLOR_RESET);
                continue;
            }
            repl_shell_tag_loop(mod_name, nav_root, sig, in);
            // nav_root pops here.
        }
        catch (const std::exception& ex)
        {
            repl_err() << COLOR_WARN << "Error: " << ex.what() << COLOR_RESET << "\n";
        }
#endif
    }
}

#ifdef ETCS_REPL_SHELL
// The local console's entry point -- "the navigator, driven by the tty".
inline void repl_shell_loop(ETCS::SignalContext& sig)
{
    repl_shell_load_history();
    ReplLineSource in = repl_tty_line_source(sig);
    repl_shell_loop_with(sig, in);
}

#ifdef __linux__
// ── Attach ──────────────────────────────────────────────────────────────────
// attach <socket> -- drive ANOTHER runtime's control socket from this one.
//
// The far end is an `etcs --listen <socket>` process. A session there IS the
// navigator, from the moment it connects -- there is no line-interpreter mode
// to opt out of any more, because the browse surface no longer executes trace
// lines at all. So what arrives over this link is that runtime's own menus,
// rendered where the entity graph is and sent here as text.
//
// FOREGROUND, one at a time. It borrows this terminal's line editor, so it
// cannot be a background job the way a detached script can -- the prompt
// itself is the thing being lent out.
//
// The REMOTE's prompt is displayed; this side prints none of its own. The far
// runtime's position in its own menus is the authority on where input is
// landing, and a local prompt would be describing the wrong process.
//
// Local escape word is `detach`. Everything else forwards verbatim, including
// `exit`, which ends the REMOTE session rather than this one.
inline void repl_shell_attach_loop(const std::string& path, ETCS::SignalContext& sig)
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        std::cerr << COLOR_WARN << "attach: socket() failed: "
                  << std::strerror(errno) << COLOR_RESET << "\n";
        return;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path))
    {
        std::cerr << COLOR_WARN << "attach: path too long ("
                  << path.size() << " >= " << sizeof(addr.sun_path) << ")"
                  << COLOR_RESET << "\n";
        ::close(fd);
        return;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        std::cerr << COLOR_WARN << "attach: connect('" << path << "') failed: "
                  << std::strerror(errno)
                  << " -- is that runtime running with --listen?"
                  << COLOR_RESET << "\n";
        ::close(fd);
        return;
    }

    // Same 300ms wake-up the far side uses, for the same reason: a quiet peer
    // must not park this loop past a Ctrl+C.
    struct timeval tv { 0, 300000 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    auto send_all = [fd](const std::string& s) -> bool
    {
        size_t off = 0;
        while (off < s.size())
        {
            ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    };

    // Waits for the remote's PROMPT, not merely for the socket to go quiet. A
    // slow command emits output in bursts with gaps between them, and stopping
    // at the first gap would print half a response and then hand back an input
    // line the remote isn't ready for -- with the rest of its output arriving
    // on top of whatever you typed next. Returns false when the peer closes.
    auto pump_until_prompt = [&]() -> bool
    {
        std::string acc;
        char buf[4096];
        while (true)
        {
            if (sig.isInterrupted() || sig.isTerminated()) return false;
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n == 0) return false;                       // peer closed
            if (n < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    continue;                               // still working -- keep waiting
                return false;
            }
            std::cout << std::string(buf, static_cast<size_t>(n)) << std::flush;
            acc.append(buf, static_cast<size_t>(n));
            if (acc.size() >= 2 && acc.compare(acc.size() - 2, 2, "> ") == 0)
                return true;
        }
    };

    ETCS_SHELL("ShellREPL", COLOR_LIB << "attached to " << path << COLOR_RESET
             << " -- 'detach' returns here, 'exit' ends the remote session.");

    if (!pump_until_prompt())
    {
        std::cerr << COLOR_WARN << "attach: remote closed immediately."
                  << COLOR_RESET << "\n";
        ::close(fd);
        return;
    }

    while (!(sig.isInterrupted() || sig.isTerminated()))
    {
        // Empty local prompt -- the remote printed its own just above.
        std::string line = repl_shell_get_input("", sig);
        if (sig.isInterrupted() || sig.isTerminated()) break;
        if (line == "detach") break;

        if (!send_all(line + "\n"))
        {
            std::cerr << COLOR_WARN << "attach: send failed -- remote gone."
                      << COLOR_RESET << "\n";
            break;
        }
        if (!pump_until_prompt())
        {
            ETCS_SHELL("ShellREPL", COLOR_WARN << "attach: remote session ended."
                     << COLOR_RESET);
            break;
        }
    }

    ::close(fd);
    ETCS_SHELL("ShellREPL", COLOR_LIB << "detached from " << path << COLOR_RESET);
}
#endif // __linux__
#endif // ETCS_REPL_SHELL

#ifdef __linux__
// ---------------------------------------------------------------------------
// repl_session_navigator — the navigator, driven by a socket instead of a tty.
// Installed into ETCS::g_session_navigator by shell_startup() and handed EVERY
// accepted control session, immediately.
//
// It used to be reached only when a session typed `shell`, because the default
// was a line interpreter running raw .etcs through parse_line. That default is
// gone: the strict grammar is for files, and a prompt is for navigating. So
// there is no mode to select any more -- connect and you are in the navigator.
//
// Output accumulates into a LogSinkGuard-backed buffer and is flushed at
// exactly the moment input is requested, which is what a terminal does
// implicitly: render, then wait. That also keeps the flush points aligned with
// the prompts, so a client reading until "> " sees whole screens.
//
// Deliberately does NOT capture asynchronous output. log_sink is thread_local
// (Log.h), so work that hops to a ThreadPool worker logs wherever that thread
// points -- the server's own console. A navigator session shows you what your
// own commands produced, not the runtime's background traffic; that stays on
// the service's stdout, which is the right place for it to be tailed.
// ---------------------------------------------------------------------------
inline void repl_session_navigator(int fd, ETCS::SignalContext& sig)
{
    std::ostringstream sink_buf;
    std::string accum;

    // MSG_NOSIGNAL as well as the process-wide SIG_IGN in shell_startup: belt
    // and braces, and it keeps the guarantee local to the call rather than
    // dependent on startup order.
    auto send_all = [fd](const std::string& s) -> bool
    {
        size_t off = 0;
        while (off < s.size())
        {
            ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    };

    ReplLineSource in = [&](const std::string& prompt, std::string& out) -> bool
    {
        // Flush whatever the last render produced, then the prompt.
        std::string pending = sink_buf.str();
        sink_buf.str(std::string());
        sink_buf.clear();
        if (!pending.empty() && !send_all(pending)) return false;
        if (!prompt.empty() && !send_all(prompt))   return false;

        // One line, possibly spanning several reads, possibly already buffered
        // from a previous one.
        while (true)
        {
            size_t nl = accum.find('\n');
            if (nl != std::string::npos)
            {
                out = accum.substr(0, nl);
                accum.erase(0, nl + 1);
                if (!out.empty() && out.back() == '\r') out.pop_back();
                return true;
            }
            if (sig.isInterrupted() || sig.isTerminated()) return false;

            char buf[4096];
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n == 0) return false;                       // peer closed
            if (n < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    continue;                               // timeout -- re-check signals
                return false;
            }
            accum.append(buf, static_cast<size_t>(n));
        }
    };

    ETCS::LogSinkGuard sink_guard(&sink_buf);
    repl_shell_loop_with(sig, in);

    // Whatever the last screen produced has no prompt following it to trigger
    // a flush, so it goes out here.
    std::string tail = sink_buf.str();
    if (!tail.empty()) send_all(tail);
}
#endif // __linux__

// shell_startup / drive_main_loop_then_exit are declared unconditionally and
// branch internally on ETCS_REPL_SHELL, so main() needs no #ifdef of its own.
// Global namespace, matching every other repl_shell_* symbol here.
inline void shell_startup()
{
#ifdef __linux__
    // A write to a socket whose peer has gone must be an ERROR, not a death.
    // Default SIGPIPE disposition terminates the process, which for a server
    // means any client hanging up mid-write takes the runtime with it -- a
    // debug session closed at the wrong instant, a dropped connection, a pane
    // killed. Every send_all in this codebase already ends with
    // `if (n <= 0) return false;`, written to unwind exactly this case; the
    // signal was killing us before that line could run.
    //
    // Process-wide rather than per-socket because it is process-wide policy:
    // there is no write in a network runtime for which "peer vanished" should
    // mean "abort". io_uring sends already report -EPIPE rather than raising.
    ::signal(SIGPIPE, SIG_IGN);

    // Installed here rather than at static-init: main() calls this before
    // anything can accept a session, and an explicit assignment beats ordering
    // games between translation units.
    ETCS::g_session_navigator = &repl_session_navigator;
#endif
#ifdef ETCS_REPL_SHELL
    repl_shell_enable_vterm();
    repl_shell_load_history();
#endif
}
 
// control_socket, when non-empty, replaces the drain wait with a control
// listener -- the headless build's substitute for stdin. Empty keeps the
// existing drain-until-finished behavior, which is still the correct mode for
// genuine batch work (run a script, wait for what it detached, exit). Ignored
// entirely by an interactive build, which already has a stream.
//
// Either branch runs shutdown_detached_executors() and
// PendingUnloadRegistry::join_all() exactly once before returning.
//
// The join fixes a reproduced SIGSEGV: main() could return, and process exit
// proceed, while a module's RequestUnloadEvent 200ms-delay recheck was still
// in flight on an untracked thread -- racing dlclose() against that module's
// own still-running workers. An empty registry (the common case) costs
// nothing.
inline int drive_main_loop_then_exit(ETCS::SignalContext& ctx, int code,
                                     const std::string& control_socket = "")
{
#ifdef ETCS_REPL_SHELL
    (void)control_socket;
    repl_shell_loop(ctx);
#else
  #ifdef __linux__
    if (!control_socket.empty())
        ETCS::run_control_listener(control_socket, ctx);
    else
        ETCS::wait_for_environment_drain(ctx);
  #else
    (void)control_socket;
    ETCS::wait_for_environment_drain(ctx);
  #endif
#endif
    ETCS::shutdown_detached_executors();
    ETCS::PendingUnloadRegistry::getInstance().join_all();
    return code;
}
 
#endif // SHELLREPL_H__
 
