#ifndef COMMAND_EXECUTOR_H__
#define COMMAND_EXECUTOR_H__
// CommandExecutor.h - execution only, no terminal I/O
// Consumes Command values produced by parse_line() and fires ETCS events.
//
// This file is deliberately not partial to terminal output -- it drives
// script execution and detached background threads just as often as an
// interactive terminal, and ANSI color codes would be garbage in any of those
// non-terminal contexts. Every log/warning here carries plain identity tokens
// ("CommandExecutor") with no coloring. A caller that DOES want colored output
// for the execution portion specifically wraps its own call into
// execute_command with an ANSI code before and a reset after.
//
// ---------------------------------------------------------------------------
// WHAT CHANGED, AND WHY THE FILE IS SMALLER
//
// Under the previous grammar this file carried the other half of the parser:
// a line's meaning depended on ambient state, so the executor had to maintain
// that state (module_name, tag_name, active_rid, pending_name,
// pending_stream), repair it (strip_leading_name_token), and guess when it was
// absent (get_or_spawn_entity's silent auto-spawn). All five are gone, and
// with them:
//
//   get_or_spawn_entity     -- there is nothing to guess. A receiver is named
//                              or the line does not parse.
//   strip_leading_name_token -- brackets ended the "is the first token a
//                              selector or an argument" question.
//   PendingStream machinery -- streams are one line, both ends.
//   resolve_stream_target,
//   resolve_inline_producer,
//   resolve_inline_consumer -- three functions that each resolved "the entity
//                              this end means, or spawn one" collapse into one
//                              resolve_receiver that only ever resolves.
//   run_socket_repl         -- the browse surface no longer executes .etcs
//                              lines. A control session gets the navigator.
//
// What is NEW is the whole-tree preflight (preflight_script_tree, below):
// resolve every detach/run target recursively, check the entire name graph,
// and refuse before line one. Nothing in the previous file did this, because
// under a grammar where meaning depended on execution order there was nothing
// decidable to check ahead of time.
// ---------------------------------------------------------------------------
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <optional>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <cstdio>
#include <algorithm>
#include <unordered_set>

#ifdef __linux__
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#include <cctype>
#include <cstring>
#endif

#include "ETCS_API.h"
#include "Command.h"

namespace ETCS {

// ---------------------------------------------------------------------------
// ExecuteStatus
//
//   Ok        the line did what it said, or reported its own refusal and the
//             trace continues. A FAILED ACTION IS NOT A STOP -- a work
//             function rejecting its payload (SetPort against an already
//             listening server) is a recorded outcome, not a broken
//             transcript, and run_tls_website.etcs depends on exactly that.
//   Exit      deliberate early stop. Not a failure.
//   Error     the line could not be carried out at all -- a parse error, an
//             unresolvable receiver. Reported; the script stops.
//   Unmet     a demand was not satisfied: a `requires` with no binding, a
//             `requires` whose tags the bound entity does not carry, or a
//             strict `attach` with nothing to attach to. The script does not
//             start, or stops at the attach.
//   Vanished  an entity in this script's own closure stopped resolving, and
//             this line named it. Stops AT that line.
//   Fatal     an action threw something the runtime could not attribute.
// ---------------------------------------------------------------------------
enum class ExecuteStatus { Ok, Exit, Error, Unmet, Vanished, Fatal };

inline const char* execute_status_name(ExecuteStatus s)
{
    switch (s)
    {
        case ExecuteStatus::Ok:       return "ok";
        case ExecuteStatus::Exit:     return "exit";
        case ExecuteStatus::Error:    return "error";
        case ExecuteStatus::Unmet:    return "unmet";
        case ExecuteStatus::Vanished: return "vanished";
        case ExecuteStatus::Fatal:    return "fatal";
    }
    return "?";
}

struct ExecuteResult
{
    ExecuteStatus status = ExecuteStatus::Ok;
    std::string   message;
};

struct ExecSource { std::string origin; size_t line_number; };

inline void exec_log(const ExecSource& src, const std::string& msg)
{
    if (src.line_number > 0)
        { ETCS_LOG("CommandExecutor", "[" << src.origin << ":" << src.line_number << "] " << msg); }
    else
        { ETCS_LOG("CommandExecutor", msg); }
}

// Follows the active ETCS_LOG sink when there is one, exactly as ETCS_LOG_2
// does, rather than always taking std::cerr. Without this a redirected
// session sees its successes and none of its failures.
inline void exec_warn(const ExecSource& src, const std::string& msg)
{
    std::ostream& out = log_sink ? *log_sink : std::cerr;
    if (src.line_number > 0)
        out << "[" << src.origin << ":" << src.line_number << "] " << msg << "\n";
    else
        out << msg << "\n";
}

#ifdef ETCS_LOADER

inline const ETCS::RIDListHandle* get_handle(const std::string& module,
                                             const std::string& tag)
{
    ETCS::Buffer key;
    key.writeString((module + ":" + tag).c_str());
    auto& ridMap = ETCS::EventNode::getInstance().ridMap;
    auto it = ridMap.find(key);
    return (it != ridMap.end()) ? &it->second : nullptr;
}

// resolve_module — takes LifetimeOwner. A Root holds ONE module_ at a time,
// so a script naming a second module would hit attachModule's already-bound
// guard and get DROPPED, surfacing much later as "no ridlist for X::Y".
// changeModule is the operation meant for this. Unconditional: changeModule
// is a documented no-op for the module already attached. Roots only -- an
// Entity's module is its type's origin, not a navigable slot.
inline bool resolve_module(const std::string& module_name,
                           const ExecSource& src, ETCS::LifetimeOwner entity)
{
    try
    {
        if (entity.kind == ETCS::LifetimeOwner::Kind::Root)
            entity.asRoot().changeModule(module_name);

        if (!ETCS::ResolveEvent{module_name.c_str(), entity}())
        {
            exec_warn(src, std::string("Module '") + module_name + "' not available.");
            return false;
        }
    }
    catch (const std::exception& ex)
    {
        exec_warn(src, std::string("Failed to load module '") + module_name + "': " + ex.what());
        return false;
    }
    return true;
}

inline bool verify_tag(ETCS::LifetimeOwner entity, const std::string& module_name,
                       const std::string& tag_name, const ExecSource& src)
{
    const auto& tags = entity.module().getTags();
    for (const auto& t : tags)
        if (t.toString() == tag_name) return true;
    exec_warn(src, std::string("Tag '") + tag_name + "' not found in module '"
              + module_name + "'.");
    return false;
}

inline ETCS::Entity* get_entity_by_rid(const std::string& module,
                                       const std::string& tag,
                                       ETCS::RID rid,
                                       const ExecSource& src)
{
    const ETCS::RIDListHandle* handle = get_handle(module, tag);
    if (!handle)
    {
        exec_warn(src, std::string("No ridlist for '") + module + "::" + tag + "'.");
        return nullptr;
    }
    return handle->invoke_get(rid);
}

// resolve_entity_anywhere — Entity* from a bare RID. Sound because RIDs are
// runtime-unique, so the first hit is the only hit.
//
// USE ONLY WHEN THE Module::Tag IS UNKNOWN. This walks every absorbed handle
// in the loader's ridMap, and a handle wraps a RIDList in its module's image.
// requestUnloadImpl now purges those rows, but a targeted get_handle touches
// one row instead of all of them -- prefer resolve_bound_entity below.
inline ETCS::Entity* resolve_entity_anywhere(ETCS::RID rid)
{
    if (rid == 0) return nullptr;
    auto& ridMap = ETCS::EventNode::getInstance().ridMap;
    for (auto& [key, handle] : ridMap)
        if (ETCS::Entity* e = handle.invoke_get(rid)) return e;
    return nullptr;
}

// resolve_bound_entity — targeted; the one to reach for.
inline ETCS::Entity* resolve_bound_entity(const NameBinding& b)
{
    if (b.rid == 0) return nullptr;
    if (!b.module.empty() && !b.tag.empty())
    {
        const ETCS::RIDListHandle* handle = get_handle(b.module, b.tag);
        return handle ? handle->invoke_get(b.rid) : nullptr;
    }
    return resolve_entity_anywhere(b.rid);
}

// spawn_entity — always creates. No name table consulted, no retarget.
// PersistentNames' silent rebind-onto-a-matching-name is gone; a script that
// wants the closure's entity says attach or ensure.
inline ETCS::Entity* spawn_entity(const std::string& module, const std::string& tag,
                                  ExecutionContext& ctx, const ExecSource& src)
{
    if (!ctx.root_entity)
    {
        exec_warn(src, "spawn: current execution context has no root_entity set -- "
                       "a top-level entry point failed to wire one in before this ran.");
        return nullptr;
    }
    if (!resolve_module(module, src, ctx.root_entity)) return nullptr;
    if (!verify_tag(ctx.root_entity, module, tag, src)) return nullptr;

    try
    {
        ETCS::LoadEvent evt{(module + ":" + tag).c_str()};
        // Lets loadImpl's vacant branch bootstrap the module against
        // ctx.root_entity before Make()'ing the real entity and transferring
        // ownership to IT via a second attachModule call.
        evt.root = ctx.root_entity;
        return evt();
    }
    catch (const std::exception& ex)
    {
        exec_warn(src, std::string("spawn: ") + ex.what());
        return nullptr;
    }
}

// make_typed_child — the receiver-scoped counterpart.
//
// Deliberately NOT addTag<T>(): that template needs the concrete type's
// header compiled into THIS binary, which would give it its own
// independently-initialized TAG_MASK/CONTRACT_TAG, disjoint from the one the
// provider module's own ETCS_TAG_DECLARE populated. "<Tag>_MakeChild" is
// dlsym-resolved off the module's own registry entry instead -- no second
// compiled copy of the type, because there IS no compiled copy here at all.
//
// Called on THIS thread, never the ordering thread: the export blocks on an
// AddTagEvent internally, which the ordering thread would have to service.
inline ETCS::Entity* make_typed_child(const std::string& module, const std::string& tag,
                                      ETCS::Entity* parent, const ExecSource& src)
{
    auto& registry = ETCS::EventNode::getInstance().stream.module_registry;
    auto it = registry.find(module);
    if (it == registry.end() || !it->second)
    {
        exec_warn(src, "spawn/ensure child: module '" + module + "' is not anchored.");
        return nullptr;
    }
    void* addr = it->second->getTagFunction(tag + "_MakeChild");
    if (!addr)
    {
        exec_warn(src, "spawn/ensure child: '" + tag + "' in " + module
                     + " exports no _MakeChild -- rebuild the module.");
        return nullptr;
    }
    using MakeChildResolver = ETCS::MakeChildFunc (*)();
    ETCS::MakeChildFunc make_child = reinterpret_cast<MakeChildResolver>(addr)();
    try { return make_child(parent); }
    catch (const std::exception& ex)
    {
        exec_warn(src, std::string("spawn/ensure child: ") + ex.what());
        return nullptr;
    }
}

// resolve_receiver — the ONE place a name becomes an entity, and so the one
// liveness check site (the old grammar had three: cursor, pending-stream
// producer, payload-resolved name).
//
// Vanished only for a RID in this script's own closure. A RID it never owned
// is an ordinary unresolvable reference.
struct ResolvedName
{
    ETCS::Entity* entity = nullptr;
    NameBinding   binding;
};

inline std::optional<ResolvedName> resolve_receiver(const std::string& name,
                                                    ExecutionContext& ctx,
                                                    const ExecSource& src)
{
    auto b = ctx.lookup(name);
    if (!b)
    {
        exec_warn(src, "'" + name + "' was never introduced in this script. A name "
                       "comes from requires, spawn, attach or ensure.");
        return std::nullopt;
    }

    ETCS::Entity* e = resolve_bound_entity(*b);
    // Retract a dead global here too, so the next line sees the name as free
    // rather than as something that resolves to nothing.
    if (!e && !ctx.introduced(name)) GlobalNames::getInstance().forget(name);
    if (!e)
    {
        if (ctx.owns(b->rid))
        {
            ctx.note_lost(b->rid);
            exec_warn(src, "'" + name + "' (RID:" + std::to_string(b->rid)
                         + ") no longer resolves -- this script depends on it, so it "
                           "stops here.");
        }
        else
        {
            exec_warn(src, "'" + name + "' (RID:" + std::to_string(b->rid)
                         + ") no longer resolves.");
        }
        return std::nullopt;
    }

    ResolvedName out;
    out.entity  = e;
    out.binding = *b;
    // A binding injected across a detach/run boundary may carry no module/tag
    // (see resolve_run_bindings); recover them from the entity itself, which
    // is the authority anyway.
    if (out.binding.tag.empty())
    {
        out.binding.module = e->getSourceModule().toString();
        out.binding.tag    = e->getSourceTag().toString();
    }
    return out;
}

// lookup_live — resolve a name through the closure, evicting a dead GLOBAL.
//
// A global whose entity is gone is a false claim, and the first lookup that
// discovers it is the right place to retract it: otherwise the name stays
// permanently spoken-for and a fresh spawn under it reads as clobbering
// something that does not exist.
//
// Locals are left alone. A dead local is this script's own closure vanishing,
// which is the Vanished rule's business, not a stale-row problem -- and if a
// name is both local and global they are different entities, so the local
// dying says nothing about the global.
inline std::optional<NameBinding> live_global(const std::string& name)
{
    auto g = GlobalNames::getInstance().find(name);
    if (!g) return std::nullopt;
    if (resolve_bound_entity(*g)) return g;

    GlobalNames::getInstance().forget(name);
    ETCS_LOG("CommandExecutor", "global '" << name << "' (RID:" << g->rid
             << ") no longer resolves -- forgetting it.");
    return std::nullopt;
}

inline std::optional<NameBinding> lookup_live(ExecutionContext& ctx,
                                              const std::string& name)
{
    auto local = ctx.names.find(name);
    if (local != ctx.names.end())
        return resolve_bound_entity(local->second)
             ? std::optional<NameBinding>(local->second) : std::nullopt;
    return live_global(name);
}

// substitute_name_tokens — @name becomes its RID; everything else is
// byte-identical. The sigil is required because payloads carry paths and free
// text that could collide with a name. An unresolved @name is left as written.
// Quoted spans are skipped: 'a @b c' is a string.
//
// The name ends at the first character that cannot be part of one, rather
// than at whitespace. A role name is an identifier, and the payload it sits
// in is an ARGUMENT LIST -- so `f(@gpu, path)` is as ordinary as
// `f(800, 600, 'title')`, which has always worked. Ending only at space/tab
// made the name "gpu," there, which resolved to nothing and was passed
// through verbatim, so the callee read a 0 RID and reported a missing
// argument -- a comma silently changing what a call means, with the error
// surfacing one layer away from the cause.
inline std::string substitute_name_tokens(const std::string& payload,
                                          ExecutionContext& ctx)
{
    std::string out;
    out.reserve(payload.size());
    bool in_single = false, in_double = false;
    size_t i = 0;
    while (i < payload.size())
    {
        char ch = payload[i];
        // Escape-aware, matching find_closing_bracket and TBuffer: `\'` inside
        // a quote is a literal apostrophe, not a boundary.
        if ((in_single || in_double) && ch == '\\' && i + 1 < payload.size())
        {
            out += ch;
            out += payload[i + 1];
            i += 2;
            continue;
        }
        if (ch == '\'' && !in_double) { in_single = !in_single; out += ch; ++i; continue; }
        if (ch == '"'  && !in_single) { in_double = !in_double; out += ch; ++i; continue; }
        if (ch != '@' || in_single || in_double) { out += ch; ++i; continue; }

        size_t start = i + 1;
        size_t end   = start;
        while (end < payload.size()
               && (std::isalnum(static_cast<unsigned char>(payload[end])) || payload[end] == '_'))
            ++end;

        std::string name = payload.substr(start, end - start);
        ETCS::RID rid = name.empty() ? 0 : ctx.resolve_name(name);
        // Unresolved: emit the sigil and the name exactly as written and
        // carry on from the delimiter, which the loop copies like any other
        // byte. No npos case to special-case any more -- end is always a
        // real index or the payload length.
        if (rid == 0) out += payload.substr(i, end - i);
        else          out += std::to_string(rid);
        i = end;
    }
    return out;
}

#endif // ETCS_LOADER

// ---------------------------------------------------------------------------
// Script path resolution — #IMPORT / #EXPORT, unchanged.
//
// Both directives are the SAME as far as the runtime is concerned: the
// interpreter doesn't care whether a tool treats the file as a stack
// (#EXPORT) or an ordinary leaf (#IMPORT), only whether a domain folder was
// declared for this file's own run/detach resolution to fall back to.
//
// A file's domain folder is a SINGLE folder, consulted only for THAT file's
// own resolution. It does not propagate to scripts it runs or detaches. Every
// file's reference space is exactly "local directory + this one domain
// folder", knowable from that file alone.
// ---------------------------------------------------------------------------
inline std::string peek_import_directive(const std::string& script_path)
{
    std::ifstream in(script_path);
    if (!in.is_open()) return "";
    std::string line;
    if (!std::getline(in, line)) return "";   // line 1: shebang
    if (!std::getline(in, line)) return "";
    if (!line.empty() && line.back() == '\r') line.pop_back();

    static const std::string kImportPrefix = "#IMPORT";
    static const std::string kExportPrefix = "#EXPORT";
    std::string prefix;
    if (line.compare(0, kImportPrefix.size(), kImportPrefix) == 0)      prefix = kImportPrefix;
    else if (line.compare(0, kExportPrefix.size(), kExportPrefix) == 0) prefix = kExportPrefix;
    else return "";

    std::string rest = line.substr(prefix.size());
    size_t s = rest.find_first_not_of(" \t");
    if (s == std::string::npos) return "";
    size_t e = rest.find_last_not_of(" \t");
    return rest.substr(s, e - s + 1);
}

// Cached for the process's lifetime -- ACE_ROOT-relative directives would
// otherwise spawn a subprocess on every single run/detach resolution.
inline std::string get_ace_root()
{
    static std::string cached;
    static std::once_flag once;
    std::call_once(once, []()
    {
        FILE* pipe = popen("ace root 2>/dev/null", "r");
        if (!pipe) return;
        char buf[4096];
        std::string out;
        while (fgets(buf, sizeof(buf), pipe) != nullptr) out += buf;
        pclose(pipe);
        size_t end = out.find_last_not_of(" \t\r\n");
        cached = (end == std::string::npos) ? "" : out.substr(0, end + 1);
    });
    return cached;
}

inline std::string resolve_ace_root_placeholder(const std::string& raw_target)
{
    static const std::string kPlaceholder = "ACE_ROOT";
    const bool is_ace_root_path =
        raw_target == kPlaceholder
        || raw_target.compare(0, kPlaceholder.size() + 1, kPlaceholder + "/") == 0;
    if (!is_ace_root_path) return raw_target;

    std::string root = get_ace_root();
    if (root.empty())
    {
        std::cerr << "ACE_ROOT-relative import could not be resolved -- "
                     "'ace root' is unavailable.\n";
        return "";
    }
    std::string remainder = (raw_target.size() > kPlaceholder.size())
        ? raw_target.substr(kPlaceholder.size() + 1)
        : "";
    if (!root.empty() && root.back() != '/') root += '/';
    return root + remainder;
}

inline std::string resolve_script_path(const std::string& origin,
                                       const std::string& script_name)
{
    size_t slash = origin.find_last_of("/\\");
    std::string script_dir = (slash != std::string::npos)
        ? origin.substr(0, slash + 1) : "./";

    std::string local_candidate = script_dir + script_name;
    { std::ifstream probe(local_candidate); if (probe.is_open()) return local_candidate; }

    std::string raw_target = peek_import_directive(origin);
    if (raw_target.empty()) return local_candidate;

    std::string import_target = resolve_ace_root_placeholder(raw_target);
    if (import_target.empty()) return local_candidate;

    std::string import_dir = (import_target.front() == '/')
        ? import_target : script_dir + import_target;
    if (!import_dir.empty() && import_dir.back() != '/') import_dir += '/';
    return import_dir + script_name;
}

inline std::string format_duration_ns(long long ns)
{
    std::ostringstream oss;
    oss << std::fixed;
    if (ns < 1'000)             { oss << ns << "ns"; }
    else if (ns < 1'000'000)    { oss << std::setprecision(2) << (ns / 1'000.0) << "µs"; }
    else if (ns < 1'000'000'000){ oss << std::setprecision(2) << (ns / 1'000'000.0) << "ms"; }
    else                        { oss << std::setprecision(3) << (ns / 1e9) << "s"; }
    return oss.str();
}

// Read the whole file before executing any of it: `requires` is collected
// across the WHOLE file, so the last line can stop the first from running.
// Affordable because there is no branching -- the file IS the execution plan.
struct ScriptLine
{
    size_t  number = 0;
    Command cmd;
};

inline bool read_script(std::istream& in, std::vector<ScriptLine>& out)
{
    std::string line;
    size_t line_number = 0;
    while (std::getline(in, line))
    {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos) continue;   // blank
        if (line[s] == '#')         continue;   // comment / directive
        out.push_back(ScriptLine{line_number, parse_line(line)});
    }
    return true;
}

// ===========================================================================
// WHOLE-TREE PREFLIGHT
//
// Invoking a script resolves its entire tree -- every detach and run target,
// recursively -- reads all of it, and checks it. Nothing executes until that
// passes.
//
// Rejects nothing that would otherwise have worked -- a consequence, not a
// hope: with no branching, "reachable" means "will execute", so a check sound
// for the file is sound for the run. It only moves failure earlier.
//
// WHAT IS CHECKED HERE (static, no module loading required -- every fact
// below is stated on the line itself):
//
//   - parse errors, anywhere in the tree
//   - a name introduced twice in one script
//   - a receiver used before anything introduced it
//   - a strict `attach` with nothing in scope to attach to
//   - type disagreement on a shared name (root spawns web as one type, a leaf
//     attaches it as another -- that attach could never resolve)
//   - a `requires` no caller satisfies, anywhere in the tree
//   - a detach/run binding naming something the launching script does not have
//   - a missing script file
//   - cycles
//
// WHAT IS DELIBERATELY NOT CHECKED HERE:
//
//   `requires` TAG LISTS -- a bare is-a marker needs a per-type query the
//   module ABI does not export, and an origin-affixed one is a fact about an
//   entity's own history, unknowable before that history happens. Both are
//   checked live in check_requirements, at a point the runtime already stops
//   at for the binding itself.
//
//   PAYLOAD CONTENTS -- free text, handed to the work function untouched. The
//   strictness here is about the entity graph, not what is in the brackets.
// ===========================================================================
struct PreflightName
{
    std::string module;
    std::string tag;
    bool        typed = false;   // false for a `requires` name: no type in the address slot
};

using PreflightScope = std::unordered_map<std::string, PreflightName>;

struct PreflightReport
{
    std::vector<std::string> problems;   // refuse
    std::vector<std::string> notes;      // annotate, but run
    bool ok() const { return problems.empty(); }
};

namespace detail {

inline std::string where(const std::string& path, size_t line)
{
    return path + ":" + std::to_string(line);
}

inline std::string type_of(const PreflightName& n)
{
    return n.typed ? (n.module + "::" + n.tag) : std::string("(untyped)");
}

// One script's own contribution, plus recursion into whatever it launches.
//
//   provided  names handed to this script on its launch line, with whatever
//             type the launching script knew for them
//   globals   the ROOT script's own names. Visible to every script in the
//             tree, at any depth, with no ancestor chain in between.
inline void preflight_one(const std::string& path,
                          const PreflightScope& provided,
                          const PreflightScope& globals,
                          std::vector<std::string> stack,
                          PreflightReport& rep,
                          bool is_root)
{
    for (const auto& seen : stack)
        if (seen == path)
        {
            std::string chain;
            for (const auto& s : stack) chain += s + " -> ";
            rep.problems.push_back("cycle: " + chain + path
                + " -- with no branching there is no base case, so a cycle is "
                  "never anything but a bug.");
            return;
        }
    stack.push_back(path);

    std::ifstream in(path);
    if (!in.is_open())
    {
        rep.problems.push_back("cannot open '" + path + "'");
        return;
    }
    std::vector<ScriptLine> lines;
    read_script(in, lines);

    // Names visible to this script. Locals shadow globals; there is nothing
    // in between.
    PreflightScope local = provided;

    // The ROOT script is not checked against the globals, because the globals
    // ARE its own names -- every line it writes would otherwise be reported as
    // shadowing itself. Children still receive the real table below; this only
    // governs what THIS script compares against.
    static const PreflightScope kNoGlobals;
    const PreflightScope& scope_globals = is_root ? kNoGlobals : globals;

    // Names this script introduces ITSELF, which is what "introduced twice"
    // means. A name handed in on the launch line, or reached as a global, was
    // not introduced here -- a `requires` naming it is the script accepting
    // it, not declaring it a second time.
    std::unordered_set<std::string> introduced_here;

    auto visible = [&](const std::string& n) -> const PreflightName*
    {
        auto it = local.find(n);
        if (it != local.end()) return &it->second;
        auto g = scope_globals.find(n);
        if (g != scope_globals.end()) return &g->second;
        return nullptr;
    };

    // mismatch_reported -- attach/ensure state a type mismatch themselves;
    //   only the duplicate PROBLEM is suppressed, the same-type shadow NOTE
    //   still fires, since a leaf reusing the root's `web` is worth saying.
    // shadow_is_the_problem -- spawn, where any clash is already refused; the
    //   note would repeat it.
    auto introduce = [&](const std::string& n, const PreflightName& pn, size_t line,
                         bool mismatch_reported = false,
                         bool shadow_is_the_problem = false)
    {
        if (!introduced_here.insert(n).second)
        {
            rep.problems.push_back(where(path, line) + ": '" + n
                + "' is introduced twice in this script.");
            return;
        }
        // A local shadowing a global of a DIFFERENT type could never have
        // meant the same entity; a same-type shadow is ordinary composition
        // (two scripts reaching for an obvious name for an obvious thing) and
        // is annotated rather than refused.
        auto g = scope_globals.find(n);
        if (!shadow_is_the_problem
            && g != scope_globals.end() && pn.typed && g->second.typed)
        {
            const bool differs = (g->second.module != pn.module || g->second.tag != pn.tag);
            if (differs && !mismatch_reported)
                rep.problems.push_back(where(path, line) + ": '" + n + "' is "
                    + type_of(pn) + " here but " + type_of(g->second)
                    + " in the root script.");
            else if (!differs)
                rep.notes.push_back(where(path, line) + ": '" + n
                    + "' shadows the root's " + type_of(g->second) + ".");
        }
        local[n] = pn;
    };

    // --- pass 1: `requires` is whole-file ---------------------------------
    // Collected before line one regardless of where it sits, so a receiver on
    // line 2 may legitimately name something `requires`'d on line 40.
    for (const auto& sl : lines)
    {
        if (const CmdRequires* r = std::get_if<CmdRequires>(&sl.cmd))
        {
            const PreflightName* have = visible(r->name);
            if (!have)
                rep.problems.push_back(where(path, sl.number) + ": requires '" + r->name
                    + "' -- nothing passed on the launch line, and the root script "
                      "introduces no such name.");
            introduce(r->name, have ? *have : PreflightName{}, sl.number);
        }
    }

    // --- pass 2: everything else, in order --------------------------------
    for (const auto& sl : lines)
    {
        if (const CmdError* e = std::get_if<CmdError>(&sl.cmd))
        {
            rep.problems.push_back(where(path, sl.number) + ": " + e->message);
            continue;
        }
        if (std::holds_alternative<CmdRequires>(sl.cmd)) continue;   // pass 1

        auto need_receiver = [&](const std::string& n)
        {
            if (!visible(n))
                rep.problems.push_back(where(path, sl.number) + ": '" + n
                    + "' was never introduced in this script.");
        };

        if (const CmdAcquire* a = std::get_if<CmdAcquire>(&sl.cmd))
        {
            PreflightName pn{a->module, a->tag, true};
            if (a->verb == AcquireVerb::Spawn)
            {
                // The static half of the runtime refusal above -- caught for
                // the whole tree before anything runs, rather than at the line.
                if (const PreflightName* clash = visible(a->name))
                    rep.problems.push_back(where(path, sl.number) + ": spawn '"
                        + a->name + "' clobbers " + type_of(*clash)
                        + " already in scope. Did you mean attach/ensure instead?");
            }
            else if (a->verb == AcquireVerb::Attach)
            {
                const PreflightName* have = visible(a->name);
                if (!have)
                    rep.problems.push_back(where(path, sl.number) + ": attach '"
                        + a->name + "' -- nothing in scope. Did you mean ensure?");
                else if (have->typed && (have->module != a->module || have->tag != a->tag))
                    rep.problems.push_back(where(path, sl.number) + ": attach '"
                        + a->name + "' as " + a->module + "::" + a->tag
                        + " but it is " + type_of(*have) + " -- that attach could "
                          "never resolve.");
            }
            else if (a->verb == AcquireVerb::Ensure)
            {
                const PreflightName* have = visible(a->name);
                if (have && have->typed && (have->module != a->module || have->tag != a->tag))
                    rep.problems.push_back(where(path, sl.number) + ": ensure '"
                        + a->name + "' as " + a->module + "::" + a->tag
                        + " but it is already " + type_of(*have) + ".");
            }
            introduce(a->name, pn, sl.number, a->verb != AcquireVerb::Spawn,
                      a->verb == AcquireVerb::Spawn);
        }
        else if (const CmdChildAcquire* ca = std::get_if<CmdChildAcquire>(&sl.cmd))
        {
            need_receiver(ca->parent_name);
            if (ca->verb == AcquireVerb::Spawn)
            {
                if (const PreflightName* clash = visible(ca->name))
                    rep.problems.push_back(where(path, sl.number) + ": "
                        + ca->parent_name + ".spawn '" + ca->name + "' clobbers "
                        + type_of(*clash) + " already in scope. Did you mean "
                        + ca->parent_name + ".attach/.ensure instead?");
            }
            if (ca->verb == AcquireVerb::Attach && !visible(ca->name))
                rep.problems.push_back(where(path, sl.number) + ": " + ca->parent_name
                    + ".attach '" + ca->name + "' -- nothing in scope to attach to.");
            introduce(ca->name, PreflightName{ca->module, ca->tag, true},
                      sl.number, ca->verb != AcquireVerb::Spawn,
                      ca->verb == AcquireVerb::Spawn);
        }
        else if (const CmdAction* act = std::get_if<CmdAction>(&sl.cmd))
        {
            need_receiver(act->receiver);
            if (act->is_stream) need_receiver(act->consumer_receiver);
        }
        else if (const CmdKill* k = std::get_if<CmdKill>(&sl.cmd))
        {
            need_receiver(k->receiver);
        }
        else if (const CmdUnflag* u = std::get_if<CmdUnflag>(&sl.cmd))
        {
            need_receiver(u->receiver);
        }
        else if (std::holds_alternative<CmdDetach>(sl.cmd)
              || std::holds_alternative<CmdRun>(sl.cmd))
        {
            const std::string& script =
                std::holds_alternative<CmdDetach>(sl.cmd)
                    ? std::get<CmdDetach>(sl.cmd).script
                    : std::get<CmdRun>(sl.cmd).script;
            const auto& bindings =
                std::holds_alternative<CmdDetach>(sl.cmd)
                    ? std::get<CmdDetach>(sl.cmd).bindings
                    : std::get<CmdRun>(sl.cmd).bindings;

            PreflightScope child_provided;
            for (const auto& [child_key, parent_key] : bindings)
            {
                const PreflightName* have = visible(parent_key);
                if (!have)
                {
                    rep.problems.push_back(where(path, sl.number) + ": binding '"
                        + child_key + "=" + parent_key + "' -- '" + parent_key
                        + "' is not a name this script has.");
                    continue;
                }
                child_provided[child_key] = *have;
            }

            std::string child_path = resolve_script_path(path, script);
            // The root's own names are the globals for the WHOLE tree, so
            // they are threaded down unchanged rather than accumulated as we
            // descend -- a script's parent's locals are deliberately not
            // visible to it.
            preflight_one(child_path, child_provided, globals, stack, rep, false);
        }
        // CmdExit: nothing to check.
    }


}

} // namespace detail

// preflight_script_tree — the entry point. Call once, before executing the
// root script; refuse to start if it does not pass.
inline PreflightReport preflight_script_tree(const std::string& root_path,
                                             const PreflightScope& launch_bindings = {})
{
    PreflightReport rep;

    // The root's own names become the globals every script below it can see.
    // Computed by reading the root once, up front, so the recursion below can
    // resolve any leaf's `attach`/`requires` against them regardless of depth.
    PreflightScope globals = launch_bindings;
    {
        std::ifstream in(root_path);
        if (!in.is_open())
        {
            rep.problems.push_back("cannot open root script '" + root_path + "'");
            return rep;
        }
        std::vector<ScriptLine> lines;
        read_script(in, lines);
        for (const auto& sl : lines)
        {
            if (const CmdAcquire* a = std::get_if<CmdAcquire>(&sl.cmd))
                globals[a->name] = PreflightName{a->module, a->tag, true};
            else if (const CmdChildAcquire* ca = std::get_if<CmdChildAcquire>(&sl.cmd))
                globals[ca->name] = PreflightName{ca->module, ca->tag, true};
            else if (const CmdRequires* r = std::get_if<CmdRequires>(&sl.cmd))
                if (!globals.count(r->name)) globals[r->name] = PreflightName{};
        }
    }

    detail::preflight_one(root_path, launch_bindings, globals, {}, rep, true);
    return rep;
}

inline void report_preflight(const PreflightReport& rep, const std::string& root_path)
{
    for (const auto& n : rep.notes)
        ETCS_LOG("CommandExecutor", "preflight note: " << n);

    if (rep.ok())
    {
        ETCS_LOG("CommandExecutor", "preflight: " << root_path
                 << " and everything it launches resolve -- starting.");
        return;
    }
    std::ostream& out = log_sink ? *log_sink : std::cerr;
    out << "preflight: refusing to run '" << root_path << "' -- "
        << rep.problems.size() << " problem(s):\n";
    for (const auto& p : rep.problems) out << "  " << p << "\n";
}

// ---------------------------------------------------------------------------
// Detached executor registry
//
// Each detached script gets its OWN local SignalContext, parented to the
// process root. This is what makes a script individually stoppable: a
// targeted terminate sets only that job's local flag, which
// isInterrupted()/isTerminated() check FIRST before consulting parent
// authority -- so siblings and the root are untouched. Global signals
// (Ctrl+C, process shutdown) still reach every job via the same parent chain.
//
// DetachedExecutor is heap-owned via unique_ptr specifically so its address
// (and therefore local_sig's, and the atomics it points at) stays stable
// across executors_ vector growth.
// ---------------------------------------------------------------------------
struct DetachedExecutor
{
    uint64_t        id = 0;
    std::string     script;
    SignalContext   local_sig;
    SignalFlag      local_interrupt{0};
    SignalFlag      local_terminate{0};
    SignalFlag      local_user1{0};
    std::thread     thread;
    std::atomic<bool> finished{false};

    DetachedExecutor()                                   = default;
    DetachedExecutor(const DetachedExecutor&)            = delete;
    DetachedExecutor& operator=(const DetachedExecutor&) = delete;
};

struct DetachedRegistry
{
    std::mutex                                     mutex_;
    std::vector<std::unique_ptr<DetachedExecutor>> executors_;
    std::atomic<uint64_t>                          next_id_{1};

    DetachedExecutor* create(const std::string& script, SignalContext* parent_sig)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto exec = std::make_unique<DetachedExecutor>();
        exec->id     = next_id_.fetch_add(1, std::memory_order_relaxed);
        exec->script = script;
        exec->local_sig.tag       = ETCS::Buffer(("detach:" + script).c_str());
        exec->local_sig.interrupt = &exec->local_interrupt;
        exec->local_sig.terminate = &exec->local_terminate;
        exec->local_sig.user1     = &exec->local_user1;

        // ACTIVE edge: the process root, ALWAYS -- never parent_sig. A
        // detached job's lifetime is dictated by its own state machine, not
        // by the frame that launched it, so the one context its chain can
        // safely terminate at is the one guaranteed to outlive every thread.
        //
        // This is also a real dangling-pointer fix. parent_sig is whatever
        // ctx.sig was at the detach site, and for a detach issued from inside
        // a `run` that is &RunSignalScope::local_sig -- a STACK local. `run`
        // is synchronous, so that frame returns as soon as the child's lines
        // are exhausted, while anything it detached keeps running: every
        // isInterrupted() from that thread afterward walked into a freed
        // frame.
        exec->local_sig.setParent(&ETCS::RootSignalContext());

        // PASSIVE edge: the detaching parent, but ONLY when its lifetime is
        // structurally guaranteed -- which here means "it is one of THIS
        // registry's own executors", since those are unique_ptr-owned and
        // live until join_all() at shutdown. That is exactly the
        // detach->detach case, and preserving it means a targeted terminate
        // still cascades to nested detached children rather than stopping one
        // hop in. Runs before push_back, so exec cannot match its own address.
        for (const auto& e : executors_)
            if (&e->local_sig == parent_sig) { exec->local_sig.setProvider(parent_sig); break; }

        DetachedExecutor* raw = exec.get();
        executors_.push_back(std::move(exec));
        return raw;
    }

    void set_thread(DetachedExecutor* exec, std::thread t)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        exec->thread = std::move(t);
    }

    bool terminate(uint64_t id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& e : executors_) if (e->id == id) { e->local_terminate = 1; return true; }
        return false;
    }

    bool interrupt(uint64_t id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& e : executors_) if (e->id == id) { e->local_interrupt = 1; return true; }
        return false;
    }

    std::vector<std::pair<uint64_t, std::string>> list()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::pair<uint64_t, std::string>> out;
        for (auto& e : executors_) out.emplace_back(e->id, e->script);
        return out;
    }

    void join_all()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& e : executors_) if (e->thread.joinable()) e->thread.join();
        executors_.clear();
    }

    bool all_finished()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& e : executors_)
            if (!e->finished.load(std::memory_order_acquire)) return false;
        return true;
    }

    static DetachedRegistry& getInstance()
    {
        static DetachedRegistry instance;
        return instance;
    }
};

// ---------------------------------------------------------------------------
// RunSignalScope — the synchronous counterpart to DetachedExecutor's
// local_sig, minus the thread and the registry bookkeeping. A `run` scope is
// gone by the time control returns to the parent line, so there is nothing to
// list or signal after the fact.
//
// Never copy/move: local_sig holds raw pointers into this object's own
// atomics, so its address must stay stable for its whole lifetime.
// ---------------------------------------------------------------------------
struct RunSignalScope
{
    SignalFlag    local_interrupt{0};
    SignalFlag    local_terminate{0};
    SignalFlag    local_user1{0};
    SignalContext local_sig;

    RunSignalScope(const std::string& script, SignalContext* parent)
    {
        local_sig.tag       = ETCS::Buffer(("run:" + script).c_str());
        local_sig.interrupt = &local_interrupt;
        local_sig.terminate = &local_terminate;
        local_sig.user1     = &local_user1;
        local_sig.setParent(parent);
    }
    RunSignalScope(const RunSignalScope&)            = delete;
    RunSignalScope& operator=(const RunSignalScope&) = delete;
};

// Forward declaration — run_script is defined below execute_command but
// referenced inside the detach lambda.
inline bool run_script(std::istream& in,
                       const std::string& origin,
                       ExecutionContext& ctx,
                       ExecuteStatus* out_status = nullptr);

#ifdef ETCS_LOADER
// ---------------------------------------------------------------------------
// resolve_run_bindings — resolves every binding against the CURRENT ctx and
// returns child_names ready to hand to a fresh ExecutionContext.
//
// Carries the whole NameBinding across, not just the RID: the launching
// script already knows what Module::Tag each name means, and the child would
// otherwise have to rediscover it. A binding whose entity is already dead is
// reported here rather than handed over as a name that resolves to nothing.
//
// Deliberately does NOT inject "root" itself. Both callers construct their
// own fresh Root, scoped to that child execution's own stack lifetime, AFTER
// this returns -- so the correct RID does not exist yet at this point. Each
// sets out["root"] immediately after constructing its Root. This is what
// makes "root" as a name always mean "whichever Root is anchoring THIS
// execution" rather than one entity threaded unchanged through every level.
// ---------------------------------------------------------------------------
inline bool resolve_run_bindings(const std::vector<std::pair<std::string,std::string>>& bindings,
                                 ExecutionContext& ctx,
                                 const ExecSource& src,
                                 std::unordered_map<std::string, NameBinding>& out)
{
    if (!ctx.root_entity)
    {
        exec_warn(src, "run/detach: current execution context has no root_entity set.");
        return false;
    }
    for (const auto& [child_key, parent_key] : bindings)
    {
        if (child_key == "root")
        {
            exec_warn(src, "run/detach: 'root' is reserved and cannot be rebound.");
            return false;
        }
        auto b = ETCS::lookup_live(ctx, parent_key);
        if (!b)
        {
            exec_warn(src, "run/detach: '" + parent_key + "' is not a name this script has.");
            return false;
        }
        NameBinding nb = *b;
        ETCS::Entity* e = resolve_bound_entity(nb);
        if (!e)
        {
            exec_warn(src, "run/detach: '" + parent_key + "' (RID:"
                         + std::to_string(nb.rid) + ") no longer resolves.");
            return false;
        }
        if (nb.tag.empty())
        {
            nb.module = e->getSourceModule().toString();
            nb.tag    = e->getSourceTag().toString();
        }
        out[child_key] = nb;
    }
    return true;
}

// ---------------------------------------------------------------------------
// check_requirements — every `requires` in a file, checked together, before
// the first line runs.
//
// Both halves of a requirement are settled here, at the one moment both are
// knowable: that the name resolves to something live, and that what it
// resolves to carries every tag the bracket listed.
//
// Entity::hasTag routes on the first character, which is exactly the split
// this needs: an upper-case name reaches the `tags` map, which holds BOTH the
// is-a markers ETCS_MAKE_INSTANCE's generated constructor writes via
// addTypeTag AND the origin-affixed entries a spawned or attached child adds.
// One call answers both kinds. Lowercase flags live in a different map
// entirely and are unreachable from here -- which is what makes a bracketed
// requirement a stable assertion rather than something `unflag` could quietly
// invalidate afterward.
//
// All failures are reported together rather than one at a time: a caller
// missing three bindings should learn that once.
// ---------------------------------------------------------------------------
inline bool check_requirements(const std::vector<ScriptLine>& lines,
                               ExecutionContext& ctx,
                               const std::string& origin)
{
    std::vector<std::string> unmet;

    for (const auto& sl : lines)
    {
        const CmdRequires* r = std::get_if<CmdRequires>(&sl.cmd);
        if (!r) continue;

        ExecSource src{origin, sl.number};
        auto b = ETCS::lookup_live(ctx, r->name);
        if (!b)
        {
            unmet.push_back("'" + r->name + "' (line " + std::to_string(sl.number)
                + ") was not passed in, and no root global provides it");
            continue;
        }
        // lookup_live already established this resolves; it only did not
        // hand back the Entity*.
        ETCS::Entity* e = resolve_bound_entity(*b);
        if (!e) continue;

        std::vector<std::string> missing;
        for (const auto& tag : r->tags)
            if (!e->hasTag(ETCS::Buffer(tag.c_str()))) missing.push_back(tag);

        if (!missing.empty())
        {
            std::string list;
            for (size_t i = 0; i < missing.size(); ++i)
                list += (i ? ", " : "") + missing[i];

            std::vector<ETCS::Buffer> carried;
            e->getTags(carried);
            std::string has;
            for (size_t i = 0; i < carried.size(); ++i)
                has += (i ? ", " : "") + carried[i].toString();

            unmet.push_back("'" + r->name + "' (line " + std::to_string(sl.number)
                + ") does not carry [" + list + "] -- RID:" + std::to_string(b->rid)
                + " carries [" + has + "]");
            continue;
        }

        // A required name is part of this script's closure exactly as a
        // spawned or attached one is: if it vanishes later and this script
        // names it again, that is a Vanished, not an ordinary miss.
        ctx.bind(r->name, *b);
    }

    if (unmet.empty()) return true;

    std::ostream& out = log_sink ? *log_sink : std::cerr;
    out << "[" << origin << "] will not run -- " << unmet.size()
        << " unmet requirement(s):\n";
    for (const auto& u : unmet) out << "  " << u << "\n";
    return false;
}
#endif // ETCS_LOADER

// ===========================================================================
// execute_command
// ===========================================================================
inline ExecuteResult execute_command(const Command& cmd,
                                     ExecutionContext& ctx,
                                     const ExecSource& src)
{
    (void)ctx;   // every arm touching it is #ifdef ETCS_LOADER; a module build reads none

    return std::visit([&](auto&& c) -> ExecuteResult
    {
        using T = std::decay_t<decltype(c)>;

        if constexpr (std::is_same_v<T, CmdExit>)
            return {ExecuteStatus::Exit, ""};

        if constexpr (std::is_same_v<T, CmdError>)
        {
            ETCS::exec_warn(src, c.message);
            return {ExecuteStatus::Error, c.message};
        }

        // Checked as a whole file before line one runs (check_requirements).
        // Reaching one during execution means it has already been satisfied.
        if constexpr (std::is_same_v<T, CmdRequires>)
            return {ExecuteStatus::Ok, ""};

        if constexpr (std::is_same_v<T, CmdAcquire>)
        {
#ifdef ETCS_LOADER
            if (ctx.introduced(c.name))
                return {ExecuteStatus::Error,
                    "'" + c.name + "' is already introduced in this script."};

            if (c.verb == AcquireVerb::Spawn)
            {
                // Overwriting globals is a real mechanism (one runtime, one
                // table, later writes win). What is refused is doing it BY
                // ACCIDENT: `spawn` means "make a new one", so a word that
                // already answers to something was almost certainly meant to
                // reach it. The scope is named because the fix differs -- a
                // local clash is a contradiction, a global one is composition.
                if (auto clash = ETCS::lookup_live(ctx, c.name))
                {
                    const bool local = ctx.introduced(c.name);
                    std::string what = clash->tag.empty()
                        ? std::string("RID:") + std::to_string(clash->rid)
                        : clash->module + "::" + clash->tag
                          + " RID:" + std::to_string(clash->rid);
                    return {ExecuteStatus::Error,
                        "spawn '" + c.name + "': clobbering " + (local ? "local" : "global")
                        + " '" + c.name + "' (" + what + "). Did you mean attach/ensure instead?"};
                }

                ETCS::Entity* e = ETCS::spawn_entity(c.module, c.tag, ctx, src);
                if (!e) return {ExecuteStatus::Error, "spawn failed."};
                ctx.bind(c.name, e->getRID(), c.module, c.tag);
                ETCS_LOG("CommandExecutor", "spawn " << c.module << "::" << c.tag
                         << " " << c.name << " -> RID:" << e->getRID());
                return {ExecuteStatus::Ok, ""};
            }

            // attach / ensure both begin by asking the closure. The ONLY
            // difference between them is what happens when the answer is no.
            auto existing = ETCS::lookup_live(ctx, c.name);
            if (existing)
            {
                ETCS::Entity* e = ETCS::resolve_bound_entity(*existing);
                if (e)
                {
                    const std::string have_mod = existing->module.empty()
                        ? e->getSourceModule().toString() : existing->module;
                    const std::string have_tag = existing->tag.empty()
                        ? e->getSourceTag().toString() : existing->tag;

                    if (have_mod != c.module || have_tag != c.tag)
                        return {ExecuteStatus::Unmet,
                            "'" + c.name + "' is " + have_mod + "::" + have_tag
                            + ", not " + c.module + "::" + c.tag + "."};

                    ctx.bind(c.name, existing->rid, have_mod, have_tag);
                    ETCS_LOG("CommandExecutor", acquire_verb_name(c.verb) << " "
                             << c.module << "::" << c.tag << " " << c.name
                             << " -> RID:" << existing->rid << " (existing)");
                    return {ExecuteStatus::Ok, ""};
                }
            }

            if (c.verb == AcquireVerb::Attach)
                return {ExecuteStatus::Unmet,
                    "attach '" + c.name + "': nothing in scope. Did you mean ensure?"};

            ETCS::Entity* e = ETCS::spawn_entity(c.module, c.tag, ctx, src);
            if (!e) return {ExecuteStatus::Error, "ensure: spawn failed."};
            ctx.bind(c.name, e->getRID(), c.module, c.tag);
            ETCS_LOG("CommandExecutor", "ensure " << c.module << "::" << c.tag
                     << " " << c.name << " -> RID:" << e->getRID() << " (new)");
#endif
            return {ExecuteStatus::Ok, ""};
        }

        if constexpr (std::is_same_v<T, CmdChildAcquire>)
        {
#ifdef ETCS_LOADER
            // Same rule as top-level spawn -- a child spawn introduces a name
            // too, and the ambiguity is the same whoever's child it is.
            if (c.verb == AcquireVerb::Spawn)
            {
                if (auto clash = ETCS::lookup_live(ctx, c.name))
                {
                    const bool local = ctx.introduced(c.name);
                    return {ExecuteStatus::Error,
                        c.parent_name + ".spawn '" + c.name + "': clobbering "
                        + (local ? "local" : "global") + " '" + c.name + "' (RID:"
                        + std::to_string(clash->rid) + "). Did you mean "
                        + c.parent_name + ".attach/.ensure instead?"};
                }
            }
            else if (ctx.introduced(c.name))
                return {ExecuteStatus::Error,
                    "'" + c.name + "' is already introduced in this script."};

            auto parent = ETCS::resolve_receiver(c.parent_name, ctx, src);
            if (!parent)
                return {ctx.lost_rid ? ExecuteStatus::Vanished : ExecuteStatus::Error,
                        "parent '" + c.parent_name + "' unavailable."};

            // The parent is part of the MATCH, not the search order: a
            // same-named child of some OTHER parent never binds here.
            if (c.verb != AcquireVerb::Spawn)
            {
                auto existing = ETCS::lookup_live(ctx, c.name);
                if (existing)
                {
                    ETCS::Entity* prior = ETCS::resolve_bound_entity(*existing);
                    if (prior && prior->getParent() == parent->entity)
                    {
                        const std::string have_tag = prior->getSourceTag().toString();
                        if (have_tag != c.tag)
                            return {ExecuteStatus::Unmet,
                                "'" + c.name + "' is a " + have_tag + " child, not a "
                                + c.tag + "."};
                        ctx.bind(c.name, existing->rid, c.module, c.tag);
                        ETCS_LOG("CommandExecutor", c.parent_name << "."
                                 << acquire_verb_name(c.verb) << " " << c.name
                                 << " -> RID:" << existing->rid << " (existing child)");
                        return {ExecuteStatus::Ok, ""};
                    }
                }
                if (c.verb == AcquireVerb::Attach)
                    return {ExecuteStatus::Unmet,
                        c.parent_name + ".attach '" + c.name + "': no such child. Did you "
                        "mean " + c.parent_name + ".ensure?"};
            }

            if (!ETCS::resolve_module(c.module, src, ctx.root_entity))
                return {ExecuteStatus::Error, "module not found: " + c.module};

            ETCS::Entity* child = ETCS::make_typed_child(c.module, c.tag,
                                                         parent->entity, src);
            if (!child) return {ExecuteStatus::Error, "child construction failed."};

            ctx.bind(c.name, child->getRID(), c.module, c.tag);
            ETCS_LOG("CommandExecutor", c.parent_name << "."
                     << acquire_verb_name(c.verb) << "(" << c.module << "::" << c.tag
                     << " " << c.name << ") -> RID:" << child->getRID());
#endif
            return {ExecuteStatus::Ok, ""};
        }

        if constexpr (std::is_same_v<T, CmdUnflag>)
        {
#ifdef ETCS_LOADER
            auto r = ETCS::resolve_receiver(c.receiver, ctx, src);
            if (!r)
                return {ctx.lost_rid ? ExecuteStatus::Vanished : ExecuteStatus::Error,
                        "unflag: receiver unavailable."};
            // removeTag routes through the SAME TagModifyEvent /
            // Scope::interruptOne path any other removal does -- if c.flag
            // names an active_scope_* label this reaches in and interrupts
            // that stream call's own SignalContext, not merely bookkeeping.
            try { r->entity->removeTag(ETCS::Buffer(c.flag.c_str())); }
            catch (const std::exception& ex)
            {
                ETCS::exec_warn(src, std::string("unflag: ") + ex.what());
                return {ExecuteStatus::Error, ex.what()};
            }
            ETCS_LOG("CommandExecutor", "unflag: removed '" << c.flag << "' from "
                     << c.receiver << " RID:" << r->binding.rid);
#endif
            return {ExecuteStatus::Ok, ""};
        }

        if constexpr (std::is_same_v<T, CmdKill>)
        {
#ifdef ETCS_LOADER
            auto r = ETCS::resolve_receiver(c.receiver, ctx, src);
            if (!r)
                return {ctx.lost_rid ? ExecuteStatus::Vanished : ExecuteStatus::Error,
                        "kill: receiver unavailable."};

            // Both forms only REQUEST. The scope leaves the registry when its
            // body actually notices and returns, so neither branch waits, and
            // a script that needs the work to have genuinely stopped has to
            // observe that some other way. Making kill block would mean
            // blocking the executor on a body that may be mid-syscall.
            if (c.has_index)
            {
                if (!r->entity->interruptScopeAt(c.label, c.index))
                {
                    ETCS::exec_warn(src, "kill: no live '" + c.label + "' at index "
                                      + std::to_string(c.index) + ".");
                    return {ExecuteStatus::Ok, ""};
                }
                ETCS_LOG("CommandExecutor", "kill: interrupt requested for "
                         << c.label << " " << c.index << " on " << c.receiver);
            }
            else
            {
                size_t n = r->entity->interruptAllOfLabel(c.label);
                if (n == 0)
                {
                    ETCS::exec_warn(src, "kill: no live '" + c.label + "' on "
                                      + c.receiver + ".");
                    return {ExecuteStatus::Ok, ""};
                }
                ETCS_LOG("CommandExecutor", "kill: interrupt requested for all "
                         << n << " live '" << c.label << "' on " << c.receiver);
            }
#endif
            return {ExecuteStatus::Ok, ""};
        }

        if constexpr (std::is_same_v<T, CmdAction>)
        {
#ifdef ETCS_LOADER
            auto r = ETCS::resolve_receiver(c.receiver, ctx, src);
            if (!r)
                return {ctx.lost_rid ? ExecuteStatus::Vanished : ExecuteStatus::Error,
                        "receiver '" + c.receiver + "' unavailable."};

            if (!ETCS::resolve_module(r->binding.module, src, ctx.root_entity))
                return {ExecuteStatus::Error, "module not found: " + r->binding.module};

            // ---- stream: one line, both ends -----------------------------
            //
            // The CONSUMER owns the frame -- DEFINE_STREAM_FUNC_CONSUME runs
            // its body inline on this thread while PRODUCE enqueues -- so the
            // pair is built on the consumer and the producer is handed in.
            // That is also what lets the two ends live on different entities,
            // and therefore different modules.
            if (c.is_stream)
            {
                auto cons = ETCS::resolve_receiver(c.consumer_receiver, ctx, src);
                if (!cons)
                    return {ctx.lost_rid ? ExecuteStatus::Vanished : ExecuteStatus::Error,
                            "consumer '" + c.consumer_receiver + "' unavailable."};

                // A property of what a stream IS, not a limit here: the pair's
                // channel is the only route to the consumer, so a payload on
                // that side assumes it is reachable outside the channel. What
                // it needs travels in what the producer sends. Refused rather
                // than dropped.
                if (!c.consumer_payload.empty())
                    return {ExecuteStatus::Error,
                        "stream: the consuming end takes no payload -- '"
                        + c.consumer_payload + "' would have to arrive through what the "
                        "producer sends."};

                ETCS::Buffer prod_buf, cons_buf, config;
                prod_buf.write((r->binding.tag + "." + c.action).c_str());
                cons_buf.write((cons->binding.tag + "." + c.consumer_action).c_str());

                std::string payload = ETCS::substitute_name_tokens(c.payload, ctx);
                if (!payload.empty()) config.write(payload.c_str());

                ETCS_LOG("CommandExecutor", c.receiver << "." << c.action
                         << " -> " << c.consumer_receiver << "." << c.consumer_action
                         << (payload.empty() ? "" : " [" + payload + "]"));

                try { cons->entity->call(r->entity, prod_buf, cons_buf, config, *ctx.sig); }
                catch (const std::exception& ex)
                {
                    ETCS::exec_warn(src, std::string("stream error: ") + ex.what());
                    return {ExecuteStatus::Error, ex.what()};
                }
                catch (...)
                {
                    ETCS::exec_warn(src, "stream crashed (unknown exception).");
                    return {ExecuteStatus::Fatal, "unknown stream exception."};
                }
                return {ExecuteStatus::Ok, ""};
            }

            // ---- ordinary action -----------------------------------------
            ETCS::Buffer act_buf;
            act_buf.write((r->binding.tag + "." + c.action).c_str());

            try
            {
                // @name becomes a RID here, so a work function taking a <rid>
                // argument (every filter/route registration) can be written by
                // name rather than by a number copied out of a log.
                std::string payload = ETCS::substitute_name_tokens(c.payload, ctx);

                ETCS::Buffer payload_buf;
                if (!payload.empty()) payload_buf.write(payload.c_str());

                ETCS_LOG("CommandExecutor", c.receiver << "." << c.action
                         << "(" << payload << ")  [" << r->binding.module
                         << "::" << r->binding.tag << " RID:" << r->binding.rid << "]");

                r->entity->call(act_buf, payload_buf, *ctx.sig);
                ETCS_LOG("CommandExecutor", "[workFunc]: " << payload_buf);
            }
            catch (const std::exception& ex)
            {
                ETCS::exec_warn(src, "action '" + c.action + "' on " + c.receiver
                                  + " (" + r->binding.module + "::" + r->binding.tag
                                  + "): " + ex.what());
                return {ExecuteStatus::Error, ex.what()};
            }
            catch (...)
            {
                ETCS::exec_warn(src, "action crashed (unknown exception).");
                return {ExecuteStatus::Fatal, "unknown action exception."};
            }
#endif
            return {ExecuteStatus::Ok, ""};
        }

        if constexpr (std::is_same_v<T, CmdDetach>)
        {
#ifdef ETCS_LOADER
            std::unordered_map<std::string, NameBinding> child_names;
            if (!ETCS::resolve_run_bindings(c.bindings, ctx, src, child_names))
                return {ExecuteStatus::Error, "detach: binding resolution failed."};
 
            std::string script_path = ETCS::resolve_script_path(src.origin, c.script);
            DetachedExecutor* exec = DetachedRegistry::getInstance().create(c.script, ctx.sig);
            uint64_t exec_id = exec->id;
            ETCS_LOG("CommandExecutor", "detach: launching " << script_path
                     << " [id:" << exec_id << "]");
 
            // Deliberately NOT capturing ctx.root_entity for the child. The
            // old version handed the parent's Root straight to the child --
            // meaning every detached script sharing one parent fought over
            // that ONE Root's single module_ slot, and attachModule's
            // already-bound guard silently dropped any second, DIFFERENT
            // module request. Whichever detached sibling ran first won the
            // shared root and the others got nothing. Each detached thread
            // now constructs its OWN fresh Root, and binds "root" to THAT
            // Root's RID.
            std::thread child_thread([script_path, child_names, exec]() mutable
            {
                std::ifstream in(script_path);
                if (!in.is_open())
                {
                    std::cerr << "[CommandExecutor] detach: could not open '"
                              << script_path << "'\n";
                    exec->finished.store(true, std::memory_order_release);
                    return;
                }
                ETCS::Root detached_root(exec->local_sig);
                child_names["root"] = NameBinding{detached_root.getRID(), "", ""};
 
                ExecutionContext child_ctx;
                child_ctx.sig         = &exec->local_sig;
                child_ctx.names       = child_names;
                child_ctx.root_entity = &detached_root;
                child_ctx.is_root     = false;   // never publishes globals
 
                // Injected RIDs count in the closure exactly as spawned ones
                // do -- tested for liveness AT CAPTURE, which is also what
                // keeps "root" out of it without special-casing the name.
                for (const auto& [n, b] : child_ctx.names)
                    if (ETCS::resolve_bound_entity(b)) child_ctx.own(b.rid);
 
                run_script(in, script_path, child_ctx);
                exec->finished.store(true, std::memory_order_release);
            });
 
            DetachedRegistry::getInstance().set_thread(exec, std::move(child_thread));
#endif
            return {ExecuteStatus::Ok, ""};
        }
 
        if constexpr (std::is_same_v<T, CmdRun>)
        {
#ifdef ETCS_LOADER
            std::unordered_map<std::string, NameBinding> child_names;
            if (!ETCS::resolve_run_bindings(c.bindings, ctx, src, child_names))
                return {ExecuteStatus::Error, "run: binding resolution failed."};
 
            std::string script_path = ETCS::resolve_script_path(src.origin, c.script);
            std::ifstream in(script_path);
            if (!in.is_open())
            {
                ETCS::exec_warn(src, "run: could not open '" + script_path + "'");
                return {ExecuteStatus::Error, "run: file not found."};
            }
 
            // Own local sig and own local Root, both scoped to exactly this
            // run's lifetime -- same reasoning as detach's, in sequential
            // form. A nested run wanting a DIFFERENT module than the parent's
            // root already has bound would otherwise be silently dropped by
            // attachModule's already-bound guard.
            RunSignalScope run_scope(c.script, ctx.sig);
            ETCS::Root run_root(run_scope.local_sig);
            child_names["root"] = NameBinding{run_root.getRID(), "", ""};
 
            ExecutionContext child_ctx;
            child_ctx.sig         = &run_scope.local_sig;
            child_ctx.names       = child_names;
            child_ctx.root_entity = &run_root;
            child_ctx.is_root     = false;
            for (const auto& [n, b] : child_ctx.names)
                if (ETCS::resolve_bound_entity(b)) child_ctx.own(b.rid);
 
            ETCS_LOG("CommandExecutor", "run: " << script_path << " (blocking)");
            auto t0 = std::chrono::steady_clock::now();
            ExecuteStatus child_status = ExecuteStatus::Ok;
            bool ok = run_script(in, script_path, child_ctx, &child_status);
            auto t1 = std::chrono::steady_clock::now();
            std::string elapsed = ETCS::format_duration_ns(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
 
            if (ok)
            {
                ETCS_LOG("CommandExecutor", "  [run] completed " << c.script
                         << " in " << elapsed);
            }
            else if (child_status == ExecuteStatus::Exit)
            {
                ETCS_LOG("CommandExecutor", "  [run] " << c.script
                         << " stopped via 'exit' after " << elapsed);
            }
            else if (child_status == ExecuteStatus::Unmet)
            {
                // The child declared a requirement this call did not satisfy.
                // Its own report already listed every one; the caller
                // continues, because a run that could not start is a recorded
                // outcome like any other refusal.
                ETCS_LOG("CommandExecutor", "  [run] " << c.script
                         << " did not start -- unmet requirements (" << elapsed << ")");
            }
            else if (child_status == ExecuteStatus::Vanished)
            {
                // Control returns to this caller at the next line. The caller
                // is not killed -- it may separately vanish on its own next
                // reference, and that is a different event.
                ETCS_LOG("CommandExecutor", "  [run] " << c.script
                         << " stopped: a dependency vanished (" << elapsed << ")");
            }
            else if (run_scope.local_sig.isInterrupted() || run_scope.local_sig.isTerminated())
            {
                ETCS_LOG("CommandExecutor", "  [run] " << c.script
                         << " interrupted after " << elapsed << " (parent unaffected)");
            }
            else
            {
                std::cerr << "  [run] " << c.script << " crashed after " << elapsed << "\n";
                return {ExecuteStatus::Fatal, "run: child script hit a fatal error."};
            }
#endif
            return {ExecuteStatus::Ok, ""};
        }
 
        return {ExecuteStatus::Error, "unhandled command type"};
    }, cmd);
}
 
// ===========================================================================
// run_script
// ===========================================================================
inline bool run_script(std::istream& in,
                       const std::string& origin,
                       ExecutionContext& ctx,
                       ExecuteStatus* out_status)
{
    if (out_status) *out_status = ExecuteStatus::Ok;
 
    std::vector<ScriptLine> lines;
    read_script(in, lines);
 
    // Parse errors stop the file before any of it runs. Reported together --
    // a file with four typos should show four, not the first one four times.
    {
        std::vector<const ScriptLine*> bad;
        for (const auto& sl : lines)
            if (std::holds_alternative<CmdError>(sl.cmd)) bad.push_back(&sl);
        if (!bad.empty())
        {
            std::ostream& out = log_sink ? *log_sink : std::cerr;
            out << "[" << origin << "] will not run -- " << bad.size()
                << " line(s) did not parse:\n";
            for (const auto* sl : bad)
                out << "  line " << sl->number << ": "
                    << std::get<CmdError>(sl->cmd).message << "\n";
            if (out_status) *out_status = ExecuteStatus::Error;
            return false;
        }
    }
 
#ifdef ETCS_LOADER
    // Whole-file, before line one. Placement of a `requires` is a readability
    // choice, not a positional rule.
    if (!check_requirements(lines, ctx, origin))
    {
        if (out_status) *out_status = ExecuteStatus::Unmet;
        return false;
    }
#endif
 
    for (const auto& sl : lines)
    {
        ExecSource src{origin, sl.number};
        ExecuteResult result = execute_command(sl.cmd, ctx, src);
 
        // A failed ACTION is not a stop -- see ExecuteStatus. These five are.
        if (result.status == ExecuteStatus::Exit
         || result.status == ExecuteStatus::Fatal
         || result.status == ExecuteStatus::Unmet
         || result.status == ExecuteStatus::Vanished
         || result.status == ExecuteStatus::Error)
        {
            if (result.status != ExecuteStatus::Exit)
            {
                exec_warn(src, std::string("stopping: ")
                    + execute_status_name(result.status)
                    + (result.message.empty() ? "" : " -- " + result.message));
            }
            if (out_status) *out_status = result.status;
            return false;
        }
 
        // Belt and braces: an arm that noticed a lost dependency without
        // returning Vanished still stops the script here rather than letting
        // the next line run against a closure it can no longer trust.
        if (ctx.lost_rid != 0)
        {
            if (out_status) *out_status = ExecuteStatus::Vanished;
            return false;
        }
    }
 
    // NOTE: detached child threads are intentionally NOT joined here.
    // run_script returning means only that THIS script's lines are exhausted;
    // anything it detached continues independently, exactly as `detach`
    // implies. They are joined at real process shutdown --
    // shutdown_detached_executors().
    return true;
}
 
// run_root_script — the top-level entry point. Preflights the whole tree,
// refuses if it does not pass, and only then executes.
//
// Separate from run_script because the preflight is a property of an
// INVOCATION, not of a file: a script reached via detach/run has already been
// checked as part of its root's tree, and re-checking it at every hop would
// re-read the same files once per edge for no new information.
inline bool run_root_script(const std::string& path,
                            ExecutionContext& ctx,
                            ExecuteStatus* out_status = nullptr)
{
    PreflightScope launch;
    for (const auto& [name, b] : ctx.names)
        launch[name] = PreflightName{b.module, b.tag, !b.tag.empty()};
 
    PreflightReport rep = preflight_script_tree(path, launch);
    report_preflight(rep, path);
    if (!rep.ok())
    {
        if (out_status) *out_status = ExecuteStatus::Unmet;
        return false;
    }
 
    std::ifstream in(path);
    if (!in.is_open())
    {
        std::cerr << "run_root_script: could not open '" << path << "'\n";
        if (out_status) *out_status = ExecuteStatus::Error;
        return false;
    }
    return run_script(in, path, ctx, out_status);
}
 
#ifdef __linux__
// ---------------------------------------------------------------------------
// The control socket.
//
// This channel executes against a live runtime, so its only authority
// boundary is filesystem permissions -- 0600 below. That is the same
// authority a terminal on this machine already carried, which is what makes
// this a relocation of an existing surface rather than a new hole. Reached by
// logging into the machine (ssh, or a local shell) and connecting to the
// socket. A TCP listener would be a different authority entirely and must
// never be added here.
//
// A session gets the NAVIGATOR, not a line interpreter. That is the
// browse/script split: someone at a prompt is exploring a live entity graph
// and invoking whole scripts, not hand-typing trace lines -- and the strict
// grammar, which refuses anything it cannot resolve statically, is exactly
// wrong for exploration. Keeping the two surfaces separate is what lets the
// script language be as strict as it now is without making the interactive
// one painful.
//
// SO_RCVTIMEO on the LISTENING fd makes accept() return EAGAIN periodically
// so the loop re-checks signals -- what lets shutdown_detached_executors()
// actually reach this thread.
// ---------------------------------------------------------------------------
 
// Filled in by ShellREPL.h's shell_startup(); null in any build without the
// navigator compiled in.
inline void (*g_session_navigator)(int fd, SignalContext& sig) = nullptr;
 
inline void run_control_session(int fd, SignalContext& session_ctx)
{
    if (!g_session_navigator)
    {
        const char* msg = "No navigator in this build -- closing.\n";
        ::send(fd, msg, std::strlen(msg), 0);
        ::close(fd);
        return;
    }
    g_session_navigator(fd, session_ctx);
    ::close(fd);
}
 
inline void run_control_listener(const std::string& path, SignalContext& sig)
{
    // A socket file left by a previous run would make bind() fail with
    // EADDRINUSE even though nothing holds it.
    ::unlink(path.c_str());
 
    int lfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0)
    {
        std::cerr << "[CommandExecutor] control listener: socket() failed: "
                  << std::strerror(errno) << "\n";
        return;
    }
 
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path))
    {
        std::cerr << "[CommandExecutor] control listener: path too long ("
                  << path.size() << " >= " << sizeof(addr.sun_path) << ")\n";
        ::close(lfd);
        return;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
 
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        std::cerr << "[CommandExecutor] control listener: bind('" << path
                  << "') failed: " << std::strerror(errno) << "\n";
        ::close(lfd);
        return;
    }
    // Owner only. Set after bind, since the socket file does not exist before it.
    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) < 0)
    {
        std::cerr << "[CommandExecutor] control listener: chmod 0600 on '" << path
                  << "' failed: " << std::strerror(errno)
                  << " -- refusing to listen on a socket whose permissions are "
                     "unknown.\n";
        ::close(lfd);
        ::unlink(path.c_str());
        return;
    }
    if (::listen(lfd, 8) < 0)
    {
        std::cerr << "[CommandExecutor] control listener: listen() failed: "
                  << std::strerror(errno) << "\n";
        ::close(lfd);
        ::unlink(path.c_str());
        return;
    }
 
    struct timeval tv { 0, 300000 };   // 300ms -- see this function's comment
    ::setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
 
    ETCS_LOG("CommandExecutor", "control listener: accepting sessions on '"
             << path << "' (0600).");
 
    uint64_t session_no = 0;
    while (!(sig.isInterrupted() || sig.isTerminated()))
    {
        int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            std::cerr << "[CommandExecutor] control listener: accept() failed: "
                      << std::strerror(errno) << "\n";
            break;
        }
 
        const std::string label = "socket:" + path + "#" + std::to_string(++session_no);
        DetachedExecutor* exec = DetachedRegistry::getInstance().create(label, &sig);
        ETCS_LOG("CommandExecutor", "control listener: session opened ["
                 << exec->id << "] " << label);
 
        std::thread session_thread([cfd, exec]()
        {
            run_control_session(cfd, exec->local_sig);
            exec->finished.store(true, std::memory_order_release);
        });
        DetachedRegistry::getInstance().set_thread(exec, std::move(session_thread));
    }
 
    ETCS_LOG("CommandExecutor", "control listener: closing '" << path << "'.");
    ::close(lfd);
    ::unlink(path.c_str());
}
#endif // __linux__
 
// wait_for_environment_drain — blocks until every detached executor has
// finished on its own, or a signal arrives. An empty registry returns
// immediately: a drain build with no work queued has nothing to wait for.
inline void wait_for_environment_drain(SignalContext& sig)
{
    ETCS_LOG("CommandExecutor",
        "Environment established -- waiting for all detached executors to "
        "finish, or an interrupt/terminate signal.");
 
    while (!DetachedRegistry::getInstance().all_finished())
    {
        if (sig.isInterrupted() || sig.isTerminated())
        {
            ETCS_LOG("CommandExecutor",
                "wait_for_environment_drain: signal received -- unblocking.");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
 
    ETCS_LOG("CommandExecutor",
        "wait_for_environment_drain: all detached executors finished.");
}
 
// shutdown_detached_executors — signals termination to every executor sharing
// the process's root signal authority and blocks until every detached thread
// has exited and been joined.
//
// Call exactly once, at real process shutdown, AFTER the root script's own
// lines are exhausted -- never from inside run_script. Calling it there was
// the original bug: it made a root script block on its own detached children
// before ever returning, defeating the entire point of `detach`.
//
// Writes g_sig_term directly rather than through any one ctx.sig --
// RootSignalContext()'s terminate slot is wired to this exact global, so
// every SignalContext whose parent chain traces back to the root observes it,
// the same path Ctrl+C already uses.
//
// GlobalNames is cleared HERE and nowhere earlier. A root script's names are
// the runtime's globals for as long as the ROOT IS RUNNING, and the root is
// still running while anything it detached is: clearing when run_script
// returns would pull the globals out from under every detached child still
// reaching for them, which is precisely the composition run_tls_website.etcs
// depends on.
inline void shutdown_detached_executors()
{
    g_sig_term = 1;
    DetachedRegistry::getInstance().join_all();
    GlobalNames::getInstance().clear();
}
 
} // namespace ETCS
 
#endif // COMMAND_EXECUTOR_H__
 
