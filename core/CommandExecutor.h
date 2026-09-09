#ifndef COMMAND_EXECUTOR_H__
#define COMMAND_EXECUTOR_H__
// CommandExecutor.h - execution and its browse surface, no terminal I/O
// Consumes Command values produced by parse_line() and fires ETCS events.
//
// TWO HALVES, and the second one arrived from ShellREPL.h (deleted). Above:
// the executor -- what an ETCS line MEANS. Below, after the namespace closes:
// the NAVIGATOR -- a browse surface that builds Command values directly and
// hands them to execute_command, which is the only thing it is a client of.
// The terminal that used to be stapled to it is a provider now
// (modules/ShellProvider), and it is reached the way every other capability in
// the system is reached: a named action on a named entity. See THE NAVIGATOR banner at the bottom of this file.
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
#include <thread>
#include <iomanip>
#include <cstdio>
#include <algorithm>
#include <unordered_set>
#include <functional>
#include <filesystem>
#include <csignal>

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

/*
 * WHO COMPILES THE EXECUTOR.
 *
 * Historically this was the loader alone -- nothing else ran scripts, so
 * everything below sat behind ETCS_LOADER. A Shell is a provider type now
 * (ShellProvider), and a type that runs scripts needs the engine compiled where
 * it lives, so the gate is on being an executor HOST rather than on being the
 * loader.
 *
 * Not opened to every module: a DSO that compiles this gets its own copies of
 * GlobalNames and DetachedRegistry (inline function-local statics, -Bsymbolic),
 * which is right for the one module hosting a shell and pure footgun for the
 * rest. ShellProvider declares itself a host; nothing else does.
 *
 * TWO OPERATIONS STAY LOADER-ONLY, because they reach machinery a module does
 * not have rather than because of policy:
 *
 *   receiver-scoped spawn   make_typed_child dlsyms <Tag>_MakeChild off
 *                           EventNode::stream.module_registry -- which only
 *                           LoaderStream has; a module's ModuleProxy does not.
 *   stream pairs (a -> b)   Entity::call(producer, ...) is itself inside
 *                           ETCS_LOADER (Entity.h).
 *
 * Both refuse with a message in a non-loader host rather than silently doing
 * nothing, so a script that needs them says so.
 */
#if defined(ETCS_LOADER) || defined(ETCS_SHELL_HOST)
    #define ETCS_EXECUTOR_HOST 1
#endif

#ifdef ETCS_EXECUTOR_HOST

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
    /*
 * THE BOOTSTRAP HOST IS NOT ALWAYS THE EXECUTION ROOT, and conflating them is
 * a bug that predates any of this -- it was simply unreachable while every
 * execution root was a bare Root.
 *
 * loadImpl's vacant branch bootstraps a module against whatever `root` names,
 * and attachModule enforces one module per entity for that entity's whole life
 * (DynamicLoader.h): asking an already-bound entity for a DIFFERENT module is
 * dropped, deliberately, because rebinding would orphan what it pointed at.
 *
 * A Root has a vacant slot and gets reconstructed on the stack whenever one is
 * needed -- which is exactly why attachModule's own comment names "a fresh
 * entity/Root" as the correct way to target a different module. But an
 * execution root that is a real ENTITY (a Shell running a script) already has
 * its own module bound, so it can never host a second one. Every `spawn` of a
 * foreign module from such a root was therefore refused before it began.
 *
 * So the bootstrap goes on a Root, which CAN migrate in place --
 * Root::changeModule, the path attachModule's own comment names. One per
 * execution and reused (ExecutionContext::spawn_host), not one per spawn: a
 * stack Root destroyed right after the load runs ~Root and vacates the module
 * if it still holds the token.
 *
 * Nothing else changes -- the created entity takes ownership itself via
 * loadImpl's second attachModule call, so what the script gets back is
 * identical either way, and a Root-rooted execution never allocates a host at
 * all and keeps its existing path exactly.
 */
    ETCS::LifetimeOwner host = ctx.root_entity;
    if (host.kind == ETCS::LifetimeOwner::Kind::Entity)
    {
        const ETCS::Module& m = host.module();
        if (m.parent != nullptr && m.parent->name != module)
        {
            if (!ctx.spawn_host)
                ctx.spawn_host = std::make_shared<ETCS::Root>(
                    ctx.sig ? *ctx.sig : ETCS::SignalContext{});
            ctx.spawn_host->changeModule(module);   // migrate in place
            host = ctx.spawn_host.get();
        }
    }

    if (!resolve_module(module, src, host)) return nullptr;
    if (!verify_tag(host, module, tag, src)) return nullptr;

    try
    {
        ETCS::LoadEvent evt{(module + ":" + tag).c_str()};
        // Lets loadImpl's vacant branch bootstrap the module against the host
        // before Make()'ing the real entity and transferring ownership to IT
        // via a second attachModule call.
        evt.root = host;
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
#ifndef ETCS_LOADER
    // module_registry lives on LoaderStream; a module's ModuleProxy has no such
    // member, so this one operation cannot be served from a module host.
    (void)module; (void)tag; (void)parent;
    exec_warn(src, "spawn/ensure child: receiver-scoped spawn needs the loader; "
                   "this host cannot dlsym a module's _MakeChild.");
    return nullptr;
#else
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
#endif  // ETCS_LOADER
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

#endif // ETCS_EXECUTOR_HOST

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

        /*
 * ACTIVE edge: the nearest CLOSURE ROOT, or the process root when there is
 * none. Never parent_sig itself.
 *
 * Never parent_sig, because a detached job's lifetime is dictated by its own
 * state machine rather than by the frame that launched it -- and because that
 * frame may be gone. parent_sig is whatever ctx.sig was at the detach site,
 * and for a detach issued from inside a `run` that is a STACK local
 * (RunSignalScope::local_sig): `run` is synchronous, so the frame returns as
 * soon as the child's lines are exhausted while anything it detached keeps
 * running, and every isInterrupted() from that thread afterward walked into a
 * freed frame.
 *
 * But the process root was too far. A job detached by a script a SHELL is
 * running belongs to that Shell's closure -- it is one of the things "drop
 * what this script started" has to be able to reach -- and parenting past the
 * boundary put it structurally outside. closureRoot() finds the boundary if
 * the detach site is inside one; the lifetime argument survives unchanged,
 * because a closure root is a Thread entity's own context and its flags come
 * from the ROOT ARENA (ThreadBase::ensureFlag), outliving every thread that
 * can read them.
 */
        const ETCS::SignalContext* closure =
            parent_sig ? parent_sig->closureRoot() : nullptr;
        exec->local_sig.setParent(closure ? closure : &ETCS::RootSignalContext());

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

#ifdef ETCS_EXECUTOR_HOST
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
#endif // ETCS_EXECUTOR_HOST

// ===========================================================================
// execute_command
// ===========================================================================
inline ExecuteResult execute_command(const Command& cmd,
                                     ExecutionContext& ctx,
                                     const ExecSource& src)
{
    (void)ctx;   // every arm touching it is #ifdef ETCS_EXECUTOR_HOST; a module build reads none

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
#ifdef ETCS_EXECUTOR_HOST
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
#ifdef ETCS_EXECUTOR_HOST
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
#ifdef ETCS_EXECUTOR_HOST
            auto r = ETCS::resolve_receiver(c.receiver, ctx, src);
            if (!r)
                return {ctx.lost_rid ? ExecuteStatus::Vanished : ExecuteStatus::Error,
                        "unflag: receiver unavailable."};
            // removeTag routes through the SAME TagModifyEvent /
            // Scope::interruptLabel path any other removal does -- if c.flag
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
#ifdef ETCS_EXECUTOR_HOST
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
#ifdef ETCS_EXECUTOR_HOST
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
#ifdef ETCS_LOADER   // Entity::call(producer, ...) is itself loader-gated (Entity.h)
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
#endif  // ETCS_LOADER -- stream pairs need Entity::call(producer, ...)

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
#ifdef ETCS_EXECUTOR_HOST
            std::unordered_map<std::string, NameBinding> child_names;
            if (!ETCS::resolve_run_bindings(c.bindings, ctx, src, child_names))
                return {ExecuteStatus::Error, "detach: binding resolution failed."};
 
            std::string script_path = ETCS::resolve_script_path(src.origin, c.script);
            /*
 * IF THIS SCRIPT IS RUNNING UNDER A THREAD, THE JOB IS ITS CHILD.
 *
 * A detached script IS a control thread, and a control thread is an entity --
 * so what should exist here is a child Thread of whatever is running us, not a
 * row in a registry beside the entity graph. Core cannot allocate one (ontology
 * depends on core, never the reverse), so it asks through IWireThread::Detach
 * and the leaf makes one of itself.
 *
 * What that buys: identity is the child's RID rather than a hand-issued
 * integer, "which jobs did this shell start" is its typed-child list, and the
 * child's signal authority is its own SignalContext -- parented on the
 * ownership edge the entity graph already maintains, rather than pinned to the
 * process root at creation and unable to follow a reparent.
 *
 * THE FALLBACK IS TRANSITIONAL, not a design. A script started by
 * run_root_script still gets a bare Root, which is not an Entity and so has no
 * Thread half -- there is nothing to detach FROM. Those keep the old
 * Root-per-child path. It goes when a Root spawning a Shell is the only way a
 * script starts.
 */
            ETCS::IWireThread* parent_thread = nullptr;
            if (ctx.root_entity
             && ctx.root_entity.kind == ETCS::LifetimeOwner::Kind::Entity)
            {
                void* tp = ctx.root_entity.asEntity()
                             .getInterfacePointer(ETCS::Buffer("Threaded"));
                if (tp) parent_thread = static_cast<ETCS::IWireThread*>(tp);
            }

            ETCS::Entity*       child_entity = nullptr;
            ETCS::SignalContext child_sig{};      // by value; see IWireThread
            if (parent_thread)
            {
                const uint64_t kid =
                    parent_thread->Detach(ETCS::Buffer(script_path.c_str()));
                if (!kid)
                    return {ExecuteStatus::Error,
                            "detach: the running thread refused to start '"
                            + c.script + "' (halted, or out of capacity)."};
                child_entity = ETCS::resolve_entity_anywhere(kid);
                if (child_entity)
                {
                    void* tp = child_entity->getInterfacePointer(ETCS::Buffer("Threaded"));
                    if (tp) child_sig = static_cast<ETCS::IWireThread*>(tp)->Signals();
                }
                ETCS_LOG("CommandExecutor", "detach: launching " << script_path
                         << " as child RID:" << kid);
            }

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
            std::thread child_thread([script_path, child_names, exec,
                                      child_entity, child_sig]() mutable
            {
                std::ifstream in(script_path);
                if (!in.is_open())
                {
                    std::cerr << "[CommandExecutor] detach: could not open '"
                              << script_path << "'\n";
                    exec->finished.store(true, std::memory_order_release);
                    return;
                }
                // The child Thread entity is this job's root when there is
                // one: everything the script spawns is then owned by the job
                // that ran it, which is what makes a session's history a
                // subtree rather than a flat list. The Root is the fallback.
                ETCS::Root detached_root(child_entity ? child_sig : exec->local_sig);

                ExecutionContext child_ctx;
                if (child_entity)
                {
                    child_names["root"] = NameBinding{child_entity->getRID(), "", ""};
                    // Address of the thread's OWN copy, so nothing here points
                    // into a frame or a shell that can go away underneath it.
                    child_ctx.sig         = &child_sig;
                    child_ctx.root_entity = child_entity;
                }
                else
                {
                    child_names["root"] = NameBinding{detached_root.getRID(), "", ""};
                    child_ctx.sig         = &exec->local_sig;
                    child_ctx.root_entity = &detached_root;
                }
                child_ctx.names   = child_names;
                child_ctx.is_root = false;   // never publishes globals
 
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
#ifdef ETCS_EXECUTOR_HOST
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
 
#ifdef ETCS_EXECUTOR_HOST
    // Whole-file, before line one. Placement of a `requires` is a readability
    // choice, not a positional rule.
    if (!check_requirements(lines, ctx, origin))
    {
        if (out_status) *out_status = ExecuteStatus::Unmet;
        return false;
    }
#endif
 
#ifdef ETCS_EXECUTOR_HOST
    // A closure ends once -- see the block below.
    bool closure_ended = false;
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
 
#ifdef ETCS_EXECUTOR_HOST
        /*
 * TOTAL CLOSURE. The rule is on the closure, not on the lines.
 *
 * A closure is the set of RIDs one entry-point script granted, plus
 * everything the scripts it detached were handed out of that set. Validity
 * comes in exactly that grouping, so the set stops being valid TOGETHER --
 * and the members that were still using it have to be wound up HERE,
 * before the rest of this file runs, rather than at process shutdown after
 * this script has already dissolved the thing they were holding.
 *
 * TWO WAYS A CLOSURE ENDS, and both are it ending, not two policies:
 *
 *   signalled   the most-global authority on the active chain was raised --
 *               the cross on a window, an explicit Close, SIGINT at the
 *               command line, a terminate. SignalContext::raiseClosure is
 *               how a work function says this; raising *ctx.interrupt says
 *               something else entirely (it ends the CALL) and cannot
 *               reach past the frame, which is why the cross used to
 *               close a window while the frame edge drawing into it kept
 *               running.
 *
 *   dissolved   an RID this script owns stopped resolving. Deleting one is
 *               a script saying so deliberately, which is why the first
 *               Delete of a teardown sequence is the end of the closure and
 *               not merely a line: `overlay.Delete()` invalidates nothing
 *               a detached member holds, but it proves the script has
 *               moved on to unmaking what it made, and `main.Delete()`
 *               three lines later is the one that faults.
 *
 * Neither ends THIS script -- the remaining lines are its own teardown and
 * must still run. Stopping is a separate question, answered by lost_rid
 * just below: winding up the closure is what a script does, being unable
 * to name a receiver it depends on is what stops it.
 *
 * Once, hence closure_ended. A closure that has been wound up is over; a
 * `detach` after it opens a new set rather than rejoining a dead one.
 *
 * The signal already reaches the members: DetachedRegistry::create parents
 * every detached local_sig to the process root, ACTIVE edge, precisely so
 * a detached script sits inside the closure of the entry-point script that
 * started it. What was missing was raising it where they could see it, and
 * then waiting for them to notice.
 *
 * Cost: the dissolved test is a resolve per owned RID, and it runs only
 * while the closure actually HAS live detached members -- a script with
 * nothing detached can invalidate whatever it likes for free, which is
 * also the only case the old "provenance, not a sweep" note (Command.h)
 * was protecting.
 *
 * Bounded, and a timeout is a warning rather than a hang: a detached member
 * that ignores its interrupt is a bug in that member, and turning it into
 * a deadlock here would hide it behind the wrong symptom.
 */
        if (ctx.is_root && !closure_ended && !DetachedRegistry::getInstance().all_finished())
        {
            const char* why = nullptr;
            if (ctx.sig && (ctx.sig->isInterrupted() || ctx.sig->isTerminated()))
                why = "signalled";

            ETCS::RID dissolved = 0;
            if (!why)
                for (ETCS::RID rid : ctx.owned_)
                    if (!ETCS::resolve_entity_anywhere(rid)) { dissolved = rid; why = "dissolved"; break; }

            if (why)
            {
                closure_ended = true;
                ETCS_LOG("CommandExecutor", "closure " << why
                         << (dissolved ? " (RID:" + std::to_string(dissolved) + " no longer resolves)"
                                       : std::string())
                         << " -- winding up detached members before the rest of "
                         << origin << " runs.");

                // A dissolved closure has not signalled anyone yet -- say so
                // through the same authority a signalled one used, so the
                // members stop for the same reason and by the same means.
                if (dissolved && ctx.sig && !ctx.sig->raiseClosureInterrupt())
                    exec_warn(src, "closure has no interrupt authority on its active chain -- "
                                   "its detached members cannot be told to stop.");

                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!DetachedRegistry::getInstance().all_finished())
                {
                    if (std::chrono::steady_clock::now() > deadline)
                    {
                        exec_warn(src, "closure drain timed out after 5s -- a detached member is "
                                       "not observing its interrupt. Continuing, but anything this "
                                       "script deletes below may still be referenced by it.");
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                DetachedRegistry::getInstance().join_all();
            }
        }
#endif

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
 
// Filled in by shell_startup() (bottom of this file); null in any build
// without the navigator compiled in.
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
 
// ===========================================================================
// THE NAVIGATOR -- the browse surface over the executor.
//
// A client of this file and of nothing else: it builds Command values directly
// and hands them to execute_command. "No terminal I/O" at the top of this file
// still holds -- nothing below reads a keystroke, sets raw mode or knows what a
// terminal is. It renders through repl_out() and asks for lines.
//
// GATING. Navigation is ETCS_LOADER-only: it reads
// LoaderStream::module_registry and the live entity graph, neither of which a
// module has. The terminal driving it is not gated at all -- whether one exists
// is a runtime question, answered by whether a Shell spawned. A remote session
// gets the SAME navigator, rendered by the side holding the entity graph rather
// than proxied to it.
//
// THE BROWSE / SCRIPT SPLIT. This surface does not execute .etcs lines, and its
// absence is what makes the script grammar affordable. A script is read long
// after it ran and benefits from refusing anything it cannot resolve
// statically; someone at a prompt is finding out what exists, and every rule
// that makes a trace trustworthy makes exploration worse -- naming a receiver
// you are still looking for, bracketing arguments to an action you have not
// found yet. So the navigator keeps its own vocabulary: bare numbers select,
// `back`/`up` move, `c0` descends into a child, `s0` interrupts by position, a
// bare action name dispatches. The two surfaces share EXECUTION, not a parser.
// ===========================================================================

#ifdef ETCS_EXECUTOR_HOST

/*
 * ANSI OR NOT IS A RUNTIME FACT, NOT A BUILD ONE.
 *
 * These were #ifdef ETCS_REPL_SHELL, which was wrong in both directions: a
 * drain build at a terminal printed no colour, an interactive build piped to a
 * file wrote escape bytes into it. Whether stdout is a terminal is the only
 * question, and it is only answerable at runtime.
 *
 * One isatty() at first use, cached, overridable. Each DSO gets its own copy of
 * the static under -Bsymbolic; they cannot disagree, since they ask the same
 * descriptor the same question.
 *
 * Each macro stays usable inside the `<<` chains that already carry it: a
 * conditional expression of two const char*, not a literal.
 */
namespace ETCS {
inline bool& color_enabled()
{
#if defined(_WIN32) || defined(_WIN64)
    static bool v = false;
#else
    static bool v = (::isatty(STDOUT_FILENO) != 0);
#endif
    return v;
}
} // namespace ETCS

#define COLOR_RESET   (ETCS::color_enabled() ? "\033[0m"    : "")
#define COLOR_DIR     (ETCS::color_enabled() ? "\033[1;36m" : "")
#define COLOR_LIB     (ETCS::color_enabled() ? "\033[1;32m" : "")
#define COLOR_WARN    (ETCS::color_enabled() ? "\033[1;33m" : "")
#define COLOR_RID     (ETCS::color_enabled() ? "\033[1;35m" : "")
#define COLOR_ACT     (ETCS::color_enabled() ? "\033[1;34m" : "")
#define COLOR_EXEC    (ETCS::color_enabled() ? "\033[0;35m" : "")

// NOTE on ETCS_LOG: the macro expands to a bare `if`, so an unbraced
// ETCS_LOG as an if/else branch body swallows the following `else`. Every
// use in a branch position below is braced for that reason.

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

/*
 * PROCESS SIGNAL MACHINERY, WHICH IS NOT TERMINAL MACHINERY.
 *
 * Both of these spent their whole life inside the tty guard, next to raw mode
 * and history, and neither has anything to do with a terminal: they read and
 * write g_sig_int/g_sig_term/g_sig_usr1, which are core/SignalContext.h
 * globals set by the process signal handler. The guard was wrong twice over --
 * it put them somewhere a drain build could not reach, and the file's own
 * comment claimed "the navigator does not reference any of it" while the
 * action loop called both.
 *
 * They exist to name an ordering once. These are ETCS::SignalFlag
 * (std::atomic), so `g_sig_int = 0` compiles as a seq_cst store and
 * `if (g_sig_int)` as a seq_cst load -- correct but stronger than needed, and
 * silently so.
 */
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

/*
 * WHO OWNS THE CONSOLE, per thread.
 *
 * The action loop clears g_sig_int around a dispatch so a Ctrl+C aimed at a
 * long action does not also drop the operator out of the shell. That is right
 * for the console and wrong for a socket session: g_sig_int is the PROCESS's
 * interrupt, and a session thread clearing it is swallowing somebody else's.
 *
 * It was hidden behind #ifdef ETCS_REPL_SHELL, which meant an interactive build
 * serving a remote session had the bug and a drain build serving the same
 * session did not -- the same surface behaving differently by build flag, which
 * is exactly what this whole split is for removing. thread_local because the
 * question is per-driver: the console loop runs on main, every session gets its
 * own thread.
 */
inline thread_local bool repl_owns_console = false;

// The Shell whose console this thread is driving, if it is driving one. Set
// beside repl_owns_console and for the same reason: `attach` borrows the
// process's line editor, so it is answerable only by the shell that holds it,
// and a socket session holds none.
inline thread_local ETCS::Entity* repl_console_shell = nullptr;

namespace ETCS {

/*
 * THE SHELL SEAM -- and there is no seam, which is the design.
 *
 * A terminal is a thing a Shell DOES, so it is four ordinary actions on the
 * Shell tag -- ReadLine, Attach, OpenConsole, CloseConsole -- reached by
 * ordinary dispatch. No export, no dlsym, no registry:
 *
 *     ETCS::Buffer data("Root> ");
 *     shell->call(ETCS::Buffer("Shell.ReadLine"), data, sig);
 *
 * WorkFunc is already `void(Entity*, Buffer& data, SignalContext)` -- buffer
 * in/out, context by value -- which is what every action in the system has
 * always taken, and Entity::call names the Buffer& overload as THE return path.
 * No wire either: `Shell` is the contract tag every OS backend unifies under
 * (Contract_ShellProvider.h), so the name is the compile-time handle.
 *
 * SYNCHRONOUS, ON THE CALLING THREAD. Entity::call -> ModuleBundle::operator()
 * -> WorkBundle::operator() -> the work function, with no queue and no ordering
 * thread in it. A ReadLine that parks for as long as the operator takes to type
 * parks main, which is the thread that should be waiting; nothing is behind it.
 *
 * THE LINE PROTOCOL. Buffer is 256 bytes, so the reply is one status byte and
 * up to 254 characters:
 *
 *     in    the prompt
 *     out   '+' <line>   a line was read; the line may legitimately be empty
 *           '-'          the source is finished (signal, stdin gone, no console)
 *           <nothing>    the action never dispatched -- also finished
 *
 * The status byte exists because pressing enter is a legitimate no-op the loop
 * must continue on, so "wrote nothing" cannot mean "empty line" -- Entity::call
 * returns void and drops the bool that would otherwise carry it.
 */
inline constexpr char SHELL_LINE_OK   = '+';
inline constexpr char SHELL_LINE_DONE = '-';

#ifdef ETCS_LOADER
/*
 * The session's Shell: spawned once, held for as long as the process runs.
 *
 * ALWAYS, IF ONE IS FOUND, and in every mode -- interactive, drain, or control
 * socket. Root is the entry point for the ontology; Shell is the entry point
 * for lifetimes and signals, so a runtime that has one has somewhere for the
 * lifetimes to hang. Only the interactive path goes on to ask it for a
 * console; the others simply keep it.
 *
 * Absence is silent and returns null. A loader with no ShellProvider beside it
 * still boots, wires entities in C++, and runs a script -- so a missing shell
 * is a fact for the caller to interpret, not an error here.
 *
 * `host` anchors the module and must outlive the returned entity, which is why
 * the caller owns it.
 */
inline ETCS::Entity* ensure_session_shell(ETCS::Root& host, ETCS::SignalContext& sig,
                                          const std::string& provider = "ShellProvider",
                                          const std::string& tag      = "Shell")
{
    ETCS::ExecutionContext env(&host, &sig);
    env.is_root = false;                  // its names are not a script's globals
    const ExecSource src{"(session shell)", 0};
    try
    {
        ETCS::Entity* shell = ETCS::spawn_entity(provider, tag, env, src);
        if (!shell) return nullptr;
        shell->call("Shell.Create", "", sig);
        return shell;
    }
    catch (const std::exception&) { return nullptr; }
}
#endif // ETCS_LOADER

} // namespace ETCS

/*
 * A Shell, as a ReplLineSource -- the mirror of repl_session_navigator's socket
 * source. Ends the loop it drives when a signal lands.
 *
 * Caches nothing beyond the entity: a Shell deleted underneath this stops
 * answering, and an unanswered call leaves the buffer untouched, which reads as
 * finished.
 */
inline ReplLineSource repl_shell_line_source(ETCS::Entity* shell, ETCS::SignalContext& sig)
{
    return [shell, &sig](const std::string& prompt, std::string& out) -> bool
    {
        if (!shell) return false;
        if (sig.isInterrupted() || sig.isTerminated()) return false;

        ETCS::Buffer data;
        data.writeString(prompt.c_str());
        shell->call(ETCS::Buffer("Shell.ReadLine"), data, sig);

        const std::string reply = data.toString();
        if (reply.empty() || reply[0] != ETCS::SHELL_LINE_OK) return false;
        out.assign(reply, 1, std::string::npos);
        return !(sig.isInterrupted() || sig.isTerminated());
    };
}


// ---------------------------------------------------------------------------
// Navigation. ETCS_LOADER only -- it reads LoaderStream::module_registry and
// the live entity graph, neither of which a module host has. The terminal it
// is driven by is no longer part of this question.
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
    ETCS_SHELL("Navigator", "\n" << COLOR_DIR << "[ " << fs::current_path().string()
             << " ]" << COLOR_RESET);
    for (const auto& entry : fs::directory_iterator("."))
    {
        if (entry.is_directory())
        {
            ETCS_SHELL("Navigator", COLOR_DIR << "  [DIR] "
                     << entry.path().filename().string() << COLOR_RESET);
        }
        else if (repl_is_module(entry))
        {
            ETCS_SHELL("Navigator", COLOR_LIB << "  [" << mods.size() << "] "
                     << entry.path().stem().string() << COLOR_RESET);
            mods.push_back(entry.path().stem().string());
        }
        else if (entry.path().extension() == ".etcs")
        {
            ETCS_SHELL("Navigator", COLOR_EXEC << "  [ETCS] "
                     << entry.path().filename().string() << COLOR_RESET);
        }
        else
        {
            ETCS_SHELL("Navigator", "        " << entry.path().filename().string());
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

    ETCS_SHELL("Navigator", COLOR_LIB
             << "  --- Live modules (loaded, not in this directory) ---" << COLOR_RESET);
    for (size_t i = 0; i < live_only.size(); ++i)
    {
        ETCS_SHELL("Navigator", COLOR_LIB << "  [" << all_mods.size() + i << "] "
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
        ETCS_SHELL("Navigator", COLOR_WARN << "Type '" << tag_name
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
        ETCS_SHELL("Navigator", "\n--- Actions for " << COLOR_DIR << tag_name
            << COLOR_RESET << " [RID:" << COLOR_RID << target_rid << COLOR_RESET << "] ---");
        for (size_t i = 0; i < action_list.size(); ++i)
        {
            const auto& [action_name, work] = action_list[i];
            ETCS_SHELL("Navigator", COLOR_ACT << "  [" << i << "] "
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
            ETCS_SHELL("Navigator", COLOR_DIR << "  [up] " << COLOR_RESET
                << parent_mod << "::" << parent_tag
                << " [RID:" << COLOR_RID << parent_rid << COLOR_RESET << "]");
        }

        std::vector<std::pair<ETCS::Buffer, ETCS::RID>> children;
        e->getTypedChildren(children);
        if (!children.empty())
        {
            ETCS_SHELL("Navigator", COLOR_DIR << "  --- Children ---" << COLOR_RESET);
            for (size_t i = 0; i < children.size(); ++i)
            {
                ETCS_SHELL("Navigator", COLOR_DIR << "  [c" << i << "] " << COLOR_RESET
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
            ETCS_SHELL("Navigator", COLOR_WARN << "  --- Active work ---" << COLOR_RESET);
            for (size_t i = 0; i < scopes.size(); ++i)
            {
                ETCS_SHELL("Navigator", COLOR_WARN << "  [s" << i << "] " << COLOR_RESET
                    << scopes[i].label << " " << COLOR_RID << scopes[i].index << COLOR_RESET
                    << (scopes[i].interrupted ? "  (stopping)" : ""));
            }
            ETCS_SHELL("Navigator",
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
            ETCS_SHELL("Navigator", COLOR_WARN << "Entity RID:" << target_rid
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
                ETCS_SHELL("Navigator", COLOR_WARN << "Parent RID:" << parent_rid
                    << " is no longer alive." << COLOR_RESET);
            }
            else
            {
                ETCS_SHELL("Navigator", COLOR_WARN << "This entity has no parent." << COLOR_RESET);
            }
            e = resolve_self();
            if (!e)
            {
                ETCS_SHELL("Navigator", COLOR_WARN << "Entity RID:" << target_rid
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
                        ETCS_SHELL("Navigator", COLOR_WARN << "Child no longer alive." << COLOR_RESET);
                    }
                }
                else
                {
                    ETCS_SHELL("Navigator", COLOR_WARN << "Invalid child index." << COLOR_RESET);
                }
            } catch (...) {
                ETCS_SHELL("Navigator", COLOR_WARN << "Invalid child selector." << COLOR_RESET);
            }
            e = resolve_self();
            if (!e)
            {
                ETCS_SHELL("Navigator", COLOR_WARN << "Entity RID:" << target_rid
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
                    ETCS_SHELL("Navigator", COLOR_WARN << "Invalid scope index." << COLOR_RESET);
                }
            } catch (...) {
                ETCS_SHELL("Navigator", COLOR_WARN << "Invalid scope selector." << COLOR_RESET);
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

        // Console only: g_sig_int is the PROCESS's interrupt, and a session
        // thread clearing it would be swallowing the local operator's.
        if (repl_owns_console) repl_clear_signal_flags();
        repl_out() << COLOR_EXEC;
        ETCS::ExecuteResult result = ETCS::execute_command(ETCS::Command{cmd}, ctx, src);
        repl_out() << COLOR_RESET;
        if (result.status == ETCS::ExecuteStatus::Fatal) return;

        if (repl_owns_console && repl_sigint_raised())
        {
            ETCS_SHELL("Navigator", COLOR_WARN << "\n[SIGNAL] Action interrupted." << COLOR_RESET);
            g_sig_int.store(0, std::memory_order_release);
        }
        e = resolve_self();
        if (!e)
        {
            ETCS_SHELL("Navigator", COLOR_WARN << "Entity RID:" << target_rid
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
            ETCS_SHELL("Navigator", COLOR_WARN << " Tag is invalid!" << COLOR_RESET);
            return;
        }

        std::vector<ETCS::RID> live_rids;
        handle->invoke_collect_rids(live_rids);

        ETCS_SHELL("Navigator", "\n--- Live instances of " << COLOR_LIB << tag_name
            << COLOR_RESET << " in " << COLOR_DIR << mod_name << COLOR_RESET
            << " [" << COLOR_RID << live_rids.size() << COLOR_RESET << " active] ---");

        if (live_rids.empty())
        {
            ETCS_SHELL("Navigator",
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
                ETCS_SHELL("Navigator", COLOR_RID << "  [" << i
                    << "] RID:" << live_rids[i] << COLOR_RESET << alias);
            }
        }
        ETCS_SHELL("Navigator",
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
                ETCS_SHELL("Navigator", COLOR_LIB << "spawned '" << sname << "' -> RID:"
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
                        ETCS_SHELL("Navigator", COLOR_WARN << "Entity dead." << COLOR_RESET);
                    }
                }
            } catch (...) {
                ETCS_SHELL("Navigator", COLOR_WARN << "Invalid index." << COLOR_RESET);
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

        ETCS_SHELL("Navigator", "\n--- Tags in " << COLOR_LIB << mod_name << COLOR_RESET << " ---");
        for (size_t i = 0; i < tags.size(); ++i)
        {
            ETCS::Buffer key;
            key.writeString((mod_name + ":" + tags[i].toString()).c_str());
            auto& ridMap = ETCS::EventNode::getInstance().ridMap;
            auto it = ridMap.find(key);
            size_t live_count = (it != ridMap.end()) ? it->second.invoke_count() : 0;
            ETCS_SHELL("Navigator", COLOR_LIB << "  [" << i << "] " << tags[i].toString()
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
                ETCS_SHELL("Navigator", COLOR_DIR
                    << "  --- Root script names (reachable by attach/ensure) ---"
                    << COLOR_RESET);
                for (auto& [name, b] : alive_named)
                {
                    ETCS_SHELL("Navigator", COLOR_RID << "  " << name << COLOR_RESET
                        << " -> " << b.tag << " RID:" << COLOR_RID << b.rid << COLOR_RESET);
                }
            }
        }

        ETCS_SHELL("Navigator", "  [back] Return   [detach] Detach   [exit] Quit");

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
        ETCS_SHELL("Navigator",
            "Detaching module: " << mod_name << " (leaving live entities running).");
    }
}

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
        repl_shell_print_dir(available_mods);
        repl_shell_print_live_modules(available_mods);
        ETCS_SHELL("Navigator", "--------------------------------------------------------");

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
                ETCS_SHELL("Navigator", COLOR_LIB << "log -> "
                         << (to_file ? "logs/<Module>.log (one file per provider)"
                                     : "this terminal")
                         << COLOR_RESET);
            }
            else if (arg.empty() || arg == "status")
            {
                ETCS_SHELL("Navigator", COLOR_DIR << "log destination: "
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
                ETCS_SHELL("Navigator", COLOR_WARN << "  (no detached scripts running)" << COLOR_RESET);
            }
            else
            {
                ETCS_SHELL("Navigator", COLOR_DIR << "--- Detached scripts ---" << COLOR_RESET);
                for (const auto& [id, script] : jobs)
                {
                    ETCS_SHELL("Navigator", COLOR_RID << "  [" << id << "] " << COLOR_RESET << script);
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
                ETCS_SHELL("Navigator", COLOR_LIB << (term ? "Terminating" : "Interrupting")
                         << " job [" << id << "]..." << COLOR_RESET);
            }
            else
            {
                repl_err() << COLOR_WARN << "signal: no job with id " << id
                           << COLOR_RESET << "\n";
            }
            continue;
        }

        // Terminal-only, and now RUNTIME-only: the attach relay borrows this
        // process's own line editor, which a session driving us does not have
        // to lend. It lives in the Shell provider for exactly that reason, so
        // the verb exists when a terminal is bound and not otherwise -- which
        // is a better answer than the old #ifdef gave, because an interactive
        // build serving a socket session used to offer it and then relay the
        // console it was not holding.
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
            if (!repl_owns_console || !repl_console_shell)
            {
                repl_err() << COLOR_WARN << "attach: no terminal on this session."
                           << COLOR_RESET << "\n";
                continue;
            }
            repl_console_shell->call("Shell.Attach",
                                     apath.substr(as, ae - as + 1).c_str(), sig);
            // Clear so a Ctrl+C aimed at the remote session doesn't also exit
            // this shell -- same reason the script path clears it.
            g_sig_int.store(0, std::memory_order_release);
            continue;
        }

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
                    repl_err() << COLOR_WARN << "Navigator: invalid injection argument '"
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
                    repl_err() << COLOR_WARN << "Navigator: invalid name '" << name
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
                        repl_err() << COLOR_WARN << "Navigator: '" << rid_str
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
                    repl_err() << COLOR_WARN << "Navigator: '" << rid_str
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
                ETCS_SHELL("Navigator", "Injected: " << name << " -> RID:" << nb.rid
                         << " (" << nb.module << "::" << nb.tag << ")");
            }
            if (!args_valid) continue;

            ETCS::run_root_script(mod_name, script_ctx);

            // Clear so a Ctrl+C aimed at the script doesn't also exit the REPL.
            g_sig_int.store(0, std::memory_order_release);
            continue;
        }

        try {
            ETCS::Root nav_root(sig);
            if (!ETCS::ResolveEvent{mod_name.c_str(), &nav_root}())
            {
                ETCS_SHELL("Navigator", COLOR_WARN << "Failed to load module: "
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
    }
}



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



// ---------------------------------------------------------------------------
// PROCESS ENTRY POINTS.
//
// shell_startup / drive_main_loop_then_exit are declared unconditionally and
// branch internally, so main() needs no #ifdef of its own. Global namespace,
// matching every other repl_* symbol here.
// ---------------------------------------------------------------------------
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
    // The terminal is NOT opened here. Vterm mode and .etcs_history belong to
    // the moment a console actually starts (Shell.OpenConsole), not to process
    // start in a binary that may never show a prompt.
}

/*
 * WHAT -DETCS_REPL_SHELL MEANS NOW.
 *
 * A LOADER flag and nothing else: "this binary should give the operator a
 * terminal". It selects no code -- the terminal is a provider either way -- so
 * the interactive and the drain binary differ in what they DO, not in what is
 * in them. With it: ask the session's Shell for a console and drive the
 * navigator from it. Without it: run the target script and drain, or accept
 * control sessions on a socket. The Shell is spawned either way.
 *
 * An interactive runtime that cannot find one has lost the thing it was built
 * to be, and falls back to drain with that stated rather than silently.
 *
 * control_socket, when non-empty, replaces the drain wait with a control
 * listener -- the headless build's substitute for stdin.
 *
 * Either branch runs shutdown_detached_executors() and
 * PendingUnloadRegistry::join_all() exactly once before returning. The join
 * fixes a reproduced SIGSEGV: main() returning let process exit race a module's
 * still-in-flight RequestUnload recheck, and its dlclose, against that module's
 * own running workers.
 */
inline int drive_main_loop_then_exit(ETCS::SignalContext& ctx, int code,
                                     const std::string& control_socket = "")
{
    // The session's Shell, spawned in EVERY mode and held until this returns.
    // Interactive asks it for a console; drain and --listen simply keep it, so
    // the runtime has its lifetime anchor whether or not anyone is typing.
    ETCS::Root    shell_host(ctx);
    ETCS::Entity* shell = ETCS::ensure_session_shell(shell_host, ctx);

#ifdef ETCS_REPL_SHELL
    (void)control_socket;
    if (shell)
    {
        ETCS::Buffer none;
        shell->call(ETCS::Buffer("Shell.OpenConsole"), none, ctx);
        repl_owns_console = true;
        repl_console_shell = shell;

        ReplLineSource in = repl_shell_line_source(shell, ctx);
        repl_shell_loop_with(ctx, in);

        repl_console_shell = nullptr;
        repl_owns_console = false;
        shell->call(ETCS::Buffer("Shell.CloseConsole"), none, ctx);
    }
    else
    {
        repl_err() << COLOR_WARN
                   << "etcs: built for an interactive terminal, but no Shell could "
                      "be spawned (ShellProvider" << DL_EXTENSION
                   << " next to the binary?). Draining instead of prompting."
                   << COLOR_RESET << "\n";
        ETCS::wait_for_environment_drain(ctx);
    }
#else
    // Held, not driven: nobody is typing, but the runtime still has its
    // lifetime anchor and anything that wants a Shell can find one.
    (void)shell;
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



#endif // ETCS_LOADER

#endif // ETCS_EXECUTOR_HOST

#endif // COMMAND_EXECUTOR_H__
 
