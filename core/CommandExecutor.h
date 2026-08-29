#ifndef COMMAND_EXECUTOR_H__
#define COMMAND_EXECUTOR_H__
// CommandExecutor.h - execution only, no terminal I/O
// Consumes Command values produced by parse_line() and fires ETCS events.
//
// This file is deliberately not partial to terminal output -- it drives
// script execution, socket sessions (run_socket_repl), and detached
// background threads just as often as an interactive terminal, and ANSI
// color codes would be garbage in any of those non-terminal contexts.
// Every log/warning here carries plain identity tokens ("CommandExecutor")
// with no coloring. A caller that DOES want colored output for the
// execution portion specifically (ShellREPL.h's interactive action loop,
// for instance) wraps its own call into execute_command with an ANSI
// color code before the call and resets it after, rather than this file
// picking a color on its own behalf.
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
#ifdef __linux__
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif
#include "ETCS_API.h"
#include "Command.h"
namespace ETCS {
enum class ExecuteStatus { Ok, Exit, Error, Fatal };
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
// does, rather than always taking std::cerr.
//
// Without this a socket session sees its successes and none of its
// failures: LogSinkGuard redirects the ETCS_LOG sink only, so every
// exec_log line reached the client while every exec_warn stayed on the
// server's stderr. Nine commands in, seven of them rejected, and the remote
// end shows nine bare prompts -- the runtime explaining itself into a
// terminal nobody was reading. Errors belong to whoever issued the command,
// which for a session is not this process's console.
//
// Sink OR cerr, not both, matching ETCS_LOG_2's own if/else. thread_local,
// so a local terminal (no sink) is unaffected and concurrent sessions never
// cross-talk.
inline void exec_warn(const ExecSource& src, const std::string& msg)
{
    std::ostream& out = log_sink ? *log_sink : std::cerr;
    if (src.line_number > 0)
        out << "[" << src.origin << ":" << src.line_number << "] "
            << msg << "\n";
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
// resolve_module — takes LifetimeOwner now, not ETCS::Entity&. Every
// actual call site passes ctx.root_entity (itself a LifetimeOwner, per
// Command.h) directly -- no more taking its address, since LifetimeOwner
// is a small tagged value, not something ResolveEvent needs a pointer
// to. ResolveEvent's own constructor takes LifetimeOwner by value too,
// so `entity` flows straight through unchanged.
inline bool resolve_module(const std::string& module_name,
                            const ExecSource& src, ETCS::LifetimeOwner entity)
{
    try
    {
        // A Root holds ONE module_ at a time, so a script naming a second
        // module hits attachModule's already-bound guard and gets DROPPED --
        // and the drop surfaces much later as "no ridlist for X::Y" rather
        // than at the context line that caused it. changeModule is the
        // operation meant for this; nothing was calling it.
        //
        // Unconditional: changeModule is a documented no-op for the module
        // already attached, so there is no name to compare against (Module
        // exposes none).
        //
        // Roots only. An Entity's module is its type's origin, not a
        // navigable slot.
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


// --- helper: mirrors CommandExecutor.h's CmdAttach ("add()" in .etcs) ---
// Deliberately NOT addTag<T>(): that template needs FileHtmlPage.h compiled
// into THIS binary, which would give it its own independently-initialized
// TAG_MASK/CONTRACT_TAG, disjoint from the one NetworkProvider.so's own
// ETCS_TAG_DECLARE populated. "<Tag>_MakeChild" is dlsym-resolved off the
// module's own registry entry instead, same as spawn_entity -- no second
// compiled copy of the type, because there IS no compiled copy here at all.
ETCS::Entity* attach_child(const std::string& module, const std::string& tag,
                            ETCS::Entity* parent)
{
    auto& registry = ETCS::EventNode::getInstance().stream.module_registry;
    auto it = registry.find(module);
    if (it == registry.end() || !it->second)
    {
        std::cerr << "attach_child: module '" << module << "' is not anchored.\n";
        return nullptr;
    }
    void* addr = it->second->getTagFunction(tag + "_MakeChild");
    if (!addr)
    {
        std::cerr << "attach_child: '" << tag << "' in " << module
                  << " exports no _MakeChild -- rebuild the module.\n";
        return nullptr;
    }
    using MakeChildResolver = ETCS::MakeChildFunc (*)();
    ETCS::MakeChildFunc make_child = reinterpret_cast<MakeChildResolver>(addr)();
    return make_child(parent);
}

// verify_tag — same treatment: LifetimeOwner instead of ETCS::Entity&.
// entity.module() dispatches through LifetimeOwner (Entity-kind or
// Root-kind, whichever this actually holds) to reach the same
// Module::getTags() call the old entity.module_.getTags() reached
// directly.
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
    ETCS::Entity* e = handle->invoke_get(rid);
    if (!e)
        exec_warn(src, std::string("Entity RID:") + std::to_string(rid) + " is no longer alive.");
    return e;
}

// resolve_entity_anywhere — Entity* from a bare RID, with no module/tag
// needed. Scans every registered RIDList, which is sound precisely because
// RIDs are runtime-unique: at most one list can ever hold a given RID, so the
// first hit is the only hit. Exists because ExecutionContext::names binds a
// name to a RID alone, while get_entity_by_rid needs the module and tag to
// find the right list up front. There can be theoretical collisions on the T type, beware! 
// Check returned entity has the tags you want. This should only ever be accessable loader side.
inline ETCS::Entity* resolve_entity_anywhere(ETCS::RID rid)
{
    if (rid == 0) return nullptr;
    auto& ridMap = ETCS::EventNode::getInstance().ridMap;
    for (auto& [key, handle] : ridMap)
        if (ETCS::Entity* e = handle.invoke_get(rid)) return e;
    return nullptr;
}
inline ETCS::Entity* spawn_entity(const std::string& module, const std::string& tag,
                                   ExecutionContext& ctx, const ExecSource& src,
                                   bool overwrite_name = false)
{
    if (!ctx.root_entity)
    {
        exec_warn(src, "Spawn error: current execution context has no root_entity set -- "
            "a top-level entry point (script-mode main(), a detach/run child, "
            "run_socket_repl) failed to wire one in before this ran.");
        return nullptr;
    }
    
    ctx.module_name = module;
    ctx.tag_name = tag;

    // Auto-retarget — if a name is already pending for this spawn, and
    // that name was bound (by an earlier run of this same script, an
    // earlier line in THIS run, or interactively via `as`) to a
    // still-alive entity of this SAME module/tag, reuse it rather than
    // spawning a duplicate. See PersistentNames' own comment (Command.h)
    // for why this check lives at process scope rather than ctx's own
    // local names table -- ctx itself is fresh every single script run,
    // so it alone could never recognize a name a PREVIOUS run created.
    //
    // "Fits the requirements of the script" is interpreted here as
    // "same module AND same tag" -- PersistentNames::find already
    // refuses to match otherwise. A name whose entity has since died
    // falls through to an ordinary fresh spawn below, which then
    // naturally overwrites the stale entry via ctx.bind (called from
    // flush_pending_name), so it self-heals without needing separate
    // cleanup.
    if (!ctx.pending_name.empty())
    {
        auto existing = PersistentNames::getInstance().find(ctx.pending_name, module, tag);
        if (existing)
        {
            ETCS::Entity* e = get_entity_by_rid(module, tag, existing->rid, src);
            if (e)
            {
                exec_log(src, "spawn: '" + ctx.pending_name + "' already exists (RID:"
                    + std::to_string(existing->rid) + ") -- retargeting instead of spawning.");
                ctx.module_name = module;
                ctx.tag_name    = tag;
                ctx.set_entity(e->getRID());
                ctx.bind(ctx.pending_name, e->getRID()); // refreshes local + persistent
                ctx.pending_name.clear();
                return e;
            }
            // Recorded RID no longer alive -- fall through to a fresh
            // spawn; the flush_pending_name call below overwrites this
            // stale entry once the new entity exists.
        }
    }
    ETCS::Entity* e = nullptr;
    try {
        ETCS::LoadEvent evt{(module + ":" + tag).c_str()};
        // Lets loadImpl's vacant branch bootstrap the module against
        // ctx.root_entity (attachModule, pass_module_to_first_child set)
        // before Make()'ing the real entity and transferring ownership to
        // IT via a second attachModule call -- the "spawn goes through the
        // Module interface first" flow, not a direct Make() that bypasses
        // it. Unused (loadImpl never reaches the vacant branch) if the
        // module's already anchored. Plain value assignment -- both
        // evt.root and ctx.root_entity are LifetimeOwner now.
        evt.root = ctx.root_entity;
        e = evt();
    }
    catch (const std::exception& ex)
    {
        exec_warn(src, std::string("Spawn error: ") + ex.what());
        return nullptr;
    }
    if (e)
    {
        ctx.module_name = module;
        ctx.tag_name    = tag;
        ctx.set_entity(e->getRID());
        ctx.flush_pending_name(e->getRID(), overwrite_name);
    }
    return e;
}


inline ETCS::Entity* get_or_spawn_entity(ExecutionContext& ctx, const ExecSource& src)
{
    if (ctx.has_entity())
        return get_entity_by_rid(ctx.module_name, ctx.tag_name, ctx.active_rid, src);
    if (!ctx.pending_name.empty())
    {
        ETCS::RID rid = ctx.resolve_name(ctx.pending_name);
        if (rid != 0)
        {
            ETCS::Entity* e = get_entity_by_rid(ctx.module_name, ctx.tag_name, rid, src);
            if (e) { ctx.set_entity(rid); ctx.pending_name.clear(); return e; }
        }
    }
    if (!ctx.tag_name.empty())
    {
        ETCS::RID rid = ctx.resolve_name(ctx.tag_name);
        if (rid != 0)
        {
            ETCS::Entity* e = get_entity_by_rid(ctx.module_name, ctx.tag_name, rid, src);
            if (e) { ctx.set_entity(rid); return e; }
        }
    }
    exec_log(src, std::string("No entity selected for '") + ctx.module_name
             + "::" + ctx.tag_name + "' -- spawning one automatically.");
    return spawn_entity(ctx.module_name, ctx.tag_name, ctx, src, false);
}


inline ETCS::Entity* resolve_stream_target(const CmdAction& c,
                                            ExecutionContext& ctx,
                                            const ExecSource& src)
{
    ETCS::RID rid = c.target_rid;
    if (rid == 0 && !c.target_name.empty())
    {
        rid = ctx.resolve_name(c.target_name);
        if (rid == 0)
        {
            exec_warn(src, "stream target name '" + c.target_name + "' not found in local names.");
            return nullptr;
        }
    }
    if (rid != 0)
        return get_entity_by_rid(c.target_module, c.target_tag, rid, src);
    ExecutionContext tmp = ctx;
    tmp.module_name  = c.target_module;
    tmp.tag_name     = c.target_tag;
    tmp.active_rid   = 0;
    tmp.pending_name = c.target_name;
    ETCS::Entity* e = spawn_entity(c.target_module, c.target_tag, tmp, src, false);
    if (e && !c.target_name.empty())
        ctx.bind(c.target_name, e->getRID());
    return e;
}
// Resolve the producer entity for an inline stream.
// target_name is the producer entity name; falls back to ambient ctx entity.
inline ETCS::Entity* resolve_inline_producer(const CmdAction& c,
                                              ExecutionContext& ctx,
                                              const ExecSource& src)
{
    if (!c.target_name.empty())
    {
        ETCS::RID rid = ctx.resolve_name(c.target_name);
        if (rid == 0)
        {
            exec_warn(src, "inline stream: producer name '" + c.target_name
                      + "' not found in local names.");
            return nullptr;
        }
        return get_entity_by_rid(c.module, c.tag, rid, src);
    }
    // No name token — use ambient entity
    return get_or_spawn_entity(ctx, src);
}


// Resolve the consumer entity for an inline stream.
// consumer_name is the consumer entity name; may equal producer entity.
inline ETCS::Entity* resolve_inline_consumer(const CmdAction& c,
                                              ExecutionContext& ctx,
                                              const ExecSource& src)
{
    if (!c.consumer_name.empty())
    {
        ETCS::RID rid = ctx.resolve_name(c.consumer_name);
        if (rid == 0)
        {
            exec_warn(src, "inline stream: consumer name '" + c.consumer_name
                      + "' not found in local names.");
            return nullptr;
        }
        return get_entity_by_rid(c.target_module, c.consumer_tag, rid, src);
    }
    // No consumer name — use ambient entity
    return get_or_spawn_entity(ctx, src);
}
// Shared by CmdDetach and CmdRun — resolves/auto-spawns every ORDINARY
// binding against the CURRENT ctx's name table and returns child_names
// ready to hand to a fresh ExecutionContext. Returns false (with a
// warning already logged) if any binding can't be resolved.
//
// Deliberately does NOT inject "root" into `out` itself. Both callers
// (CmdDetach/CmdRun) construct their own fresh ETCS::Root, scoped to
// exactly that child execution's own stack lifetime, AFTER this function
// returns -- so the correct RID to bind "root" to doesn't exist yet at
// this point. Each caller sets out["root"] itself, immediately after
// constructing its own Root, right before building the child
// ExecutionContext (see CmdDetach/CmdRun below). This is what makes
// "root" as a NAME always mean "whichever Root is anchoring THIS
// execution context" rather than some single entity threaded unchanged
// through every nested detach/run: two independent .etcs scripts (or two
// nested run/detach levels of the very same script) each get their own
// Root and their own distinct "root" RID, never the same one by
// accident, and that Root's lifetime is scoped to exactly the execution
// context's own stack -- never outliving it, never shared beyond it
// unless a script explicitly passes it along as a binding value.
//
// The reserved-name check below (rejecting a script's own attempt to
// bind something named "root" as a bindings key) is independent of WHEN
// out["root"] itself gets populated -- it guards the bindings list the
// .etcs script itself wrote, not this function's own bookkeeping.
inline bool resolve_run_bindings(const std::vector<std::pair<std::string,std::string>>& bindings,
                                  ExecutionContext& ctx,
                                  const ExecSource& src,
                                  std::unordered_map<std::string, ETCS::RID>& out)
{

    if (!ctx.root_entity)
    {
        exec_warn(src, "run/detach: current execution context has no root_entity set -- "
                        "this indicates a genuine top-level entry point (run_socket_repl, "
                        "the initial script) failed to wire one in.");
        return false;
    }
    for (const auto& [child_key, parent_key] : bindings)
    {
        if (child_key == "root")
        {
            exec_warn(src, "run/detach: 'root' is reserved and cannot be rebound -- "
                            "it always refers to whichever Root entity is scoped to "
                            "this execution.");
            return false;
        }
        ETCS::RID rid = ctx.resolve_name(parent_key);
        if (rid == 0)
        {
            // Name not yet bound — check if it matches pending_name
            // and we have enough context to auto-spawn
            if (parent_key == ctx.pending_name && ctx.has_module() && ctx.has_tag())
            {
                ETCS::Entity* e = spawn_entity(ctx.module_name, ctx.tag_name, ctx, src, false);
                if (!e)
                {
                    exec_warn(src, "run/detach: auto-spawn failed for binding: " + parent_key);
                    return false;
                }
                rid = e->getRID();
            }
            else
            {
                exec_warn(src, "run/detach: name '" + parent_key
                          + "' not found in current name table");
                return false;
            }
        }
        out[child_key] = rid;
    }
    return true;
}
// substitute_name_tokens — replaces every @name token in a payload with the
// RID that name is bound to, leaving everything else byte-identical.
//
// The sigil is deliberate rather than substituting any token that happens to
// match a bound name: payloads legitimately carry paths, titles and raw text
// that could collide with a name, and a silent numeric substitution there
// would be near-impossible to spot -- especially since TBuffer's numeric
// operator>> skips a non-numeric token silently rather than failing (see
// strip_leading_name_token's own comment). An unresolved @name is left as
// written so the receiving work function's own guard reports it.
//
// Quoted spans are skipped entirely: 'a @b c' is a string, not a reference.
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
        if (ch == '\'' && !in_double) { in_single = !in_single; out += ch; ++i; continue; }
        if (ch == '"'  && !in_single) { in_double = !in_double; out += ch; ++i; continue; }
        if (ch != '@' || in_single || in_double) { out += ch; ++i; continue; }
        size_t start = i + 1;
        size_t end   = payload.find_first_of(" \t", start);
        std::string name = payload.substr(start, (end == std::string::npos)
                                                  ? std::string::npos : end - start);
        ETCS::RID rid = name.empty() ? 0 : ctx.resolve_name(name);
        if (rid == 0) out += payload.substr(i, (end == std::string::npos)
                                                ? std::string::npos : end - i);
        else          out += std::to_string(rid);
        if (end == std::string::npos) break;
        i = end;
    }
    return out;
}
// strip_leading_name_token — if payload's first whitespace-delimited
// token resolves (via ctx's own name table) to check_rid, returns the
// payload with that token and one following delimiter removed;
// otherwise returns payload unchanged. Shared by every dispatch path
// that accepts the "Action <name> <args...>" shorthand -- previously
// only the non-stream action path stripped this, which is exactly what
// let a stream config (Listen's own payload, in particular) silently
// carry an unstripped leading selector token straight through to the
// work function's own typed parsing. Masked in practice for Listen
// specifically only because a non-numeric leading token gets silently
// skipped by TBuffer's own numeric operator>> rather than rejected, and
// a hardcoded default port that happened to match whatever was actually
// requested hid the resulting misparse entirely.
inline std::string strip_leading_name_token(const std::string& payload,
                                             ExecutionContext& ctx,
                                             ETCS::RID check_rid)
{
    if (payload.empty()) return payload;
    size_t sp = payload.find_first_of(" \t");
    std::string first_tok = (sp == std::string::npos) ? payload : payload.substr(0, sp);
    ETCS::RID tok_rid = ctx.resolve_name(first_tok);
    if (tok_rid == 0 || tok_rid != check_rid) return payload;
    std::string rest = (sp == std::string::npos) ? "" : payload.substr(sp + 1);
    size_t rs = rest.find_first_not_of(" \t");
    return (rs == std::string::npos) ? "" : rest.substr(rs);
}
#endif // ETCS_LOADER

// Peek a .etcs file's second line (the one right after the shebang) for a
// domain-folder directive: "#IMPORT <path>" or "#EXPORT <path>". Both are
// the SAME directive as far as the runtime is concerned -- the interpreter
// doesn't care whether etcs_viewer.py treats the file as a stack (#EXPORT)
// or an ordinary leaf script (#IMPORT); it only cares whether a domain
// folder was declared for this file's own run/detach resolution to fall
// back to. That tool-level stack/leaf distinction lives entirely in
// etcs_viewer.py's is_export_file, not here.
//
// This is what makes an exported stack still resolve its own layer
// scripts correctly when it's actually invoked through a symlink (e.g.
// etcs_viewer.py's central exports/ directory): the export's own
// "#EXPORT <real_home_folder>" line means resolve_script_path below
// falls back to the real source folder regardless of where the symlink
// itself lives, rather than looking for layer scripts next to the
// symlink and finding nothing.
//
// Returns "" if no domain-folder directive is present (including a bare
// "#EXPORT" with no path, any other comment, blank, or a real script
// line).
//
// A file's domain folder is a SINGLE folder, consulted only for THAT
// file's own run/detach script-name resolution. It does not propagate to
// scripts it runs or detaches, and it does not itself walk any further
// directive the target folder's own files might separately declare. This
// is deliberate: every file's reference space is exactly "local directory
// + this one domain folder", knowable from that file alone. As nested
// run/detach calls move execution from script to script, each hop
// resolves independently against its own single directive, if any — so
// the only thing "singly linked" about the whole arrangement is that no
// file can ever name more than one folder, not that folders chain
// together at resolution time.
inline std::string peek_import_directive(const std::string& script_path)
{
    std::ifstream in(script_path);
    if (!in.is_open()) return "";
    std::string line;
    // Line 1: shebang (or whatever a script opens with) — always skipped,
    // matching the position both directives occupy.
    if (!std::getline(in, line)) return "";
    if (!std::getline(in, line)) return "";
    if (!line.empty() && line.back() == '\r') line.pop_back();
    static const std::string kImportPrefix = "#IMPORT";
    static const std::string kExportPrefix = "#EXPORT";
    std::string prefix;
    if (line.compare(0, kImportPrefix.size(), kImportPrefix) == 0)
        prefix = kImportPrefix;
    else if (line.compare(0, kExportPrefix.size(), kExportPrefix) == 0)
        prefix = kExportPrefix;
    else
        return "";
    std::string rest = line.substr(prefix.size());
    size_t s = rest.find_first_not_of(" \t");
    if (s == std::string::npos) return "";
    size_t e = rest.find_last_not_of(" \t");
    return rest.substr(s, e - s + 1);
}

// Resolve the ace root directory via the 'ace root' shell command,
// mirroring etcs_viewer.py's own get_ace_root(). Cached for the process's
// entire lifetime via call_once -- ACE_ROOT-relative directives would
// otherwise spawn a subprocess on every single run/detach resolution,
// which is far too hot a path for that. popen/pclose match this file's
// existing POSIX-only orientation (sys/socket.h, sig_atomic_t, etc. are
// already unconditional elsewhere in this header).
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
// Resolve an "ACE_ROOT/..." (or bare "ACE_ROOT") directive target against
// the live 'ace root' output. Returns raw_target UNCHANGED if it isn't an
// ACE_ROOT-relative path at all (so ordinary absolute/relative directives
// keep working exactly as before). Returns "" if it IS an ACE_ROOT path
// but 'ace root' is unavailable -- the caller treats that the same as "no
// directive present", falling back to local-directory-only resolution
// rather than failing outright.
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
        ? raw_target.substr(kPlaceholder.size() + 1) // strip "ACE_ROOT/"
        : "";
    if (!root.empty() && root.back() != '/') root += '/';
    return root + remainder;
}


// Resolve a run/detach script name the same way for both commands: check
// the calling script's own directory first, then fall back to that
// script's single #IMPORT target (if it declared one) only when the name
// isn't found locally. A relative import path is resolved against the
// CALLING script's own directory; an absolute path (leading '/') is used
// as-is. Returns the local-directory candidate unchanged when no import
// directive is present, so behavior for every existing script that
// doesn't use #IMPORT is byte-for-byte identical to before.
inline std::string resolve_script_path(const std::string& origin,
                                        const std::string& script_name)
{
    size_t slash = origin.find_last_of("/\\");
    std::string script_dir = (slash != std::string::npos)
        ? origin.substr(0, slash + 1) : "./";
    std::string local_candidate = script_dir + script_name;
    {
        std::ifstream probe(local_candidate);
        if (probe.is_open()) return local_candidate;
    }
    std::string raw_target = peek_import_directive(origin);
    if (raw_target.empty()) return local_candidate;
    std::string import_target = resolve_ace_root_placeholder(raw_target);
    if (import_target.empty()) return local_candidate; // ACE_ROOT declared but unresolvable
    std::string import_dir = (import_target.front() == '/')
        ? import_target
        : script_dir + import_target;
    if (!import_dir.empty() && import_dir.back() != '/') import_dir += '/';
    return import_dir + script_name;
}
// Human-scaled duration formatting for `run` timing output. Not gated on
// ETCS_LOADER — pure formatting, no engine dependency.
inline std::string format_duration_ns(long long ns)
{
    std::ostringstream oss;
    oss << std::fixed;
    if (ns < 1'000)
        { oss << ns << "ns"; }
    else if (ns < 1'000'000)
        { oss << std::setprecision(2) << (ns / 1'000.0) << "\u00b5s"; }
    else if (ns < 1'000'000'000)
        { oss << std::setprecision(2) << (ns / 1'000'000.0) << "ms"; }
    else
        { oss << std::setprecision(3) << (ns / 1e9) << "s"; }
    return oss.str();
}


// Forward declaration — run_script is defined below execute_command but
// referenced inside the detach lambda which is a non-dependent context.
// out_status, when non-null, is set to the ExecuteStatus that caused an
// early return (Exit or Fatal) — lets a caller like CmdRun distinguish a
// deliberate `exit` from a genuine crash without re-parsing anything.
inline bool run_script(std::istream& in,
                       const std::string& origin,
                       ExecutionContext& ctx,
                       ExecuteStatus* out_status = nullptr);
// ---------------------------------------------------------------------------
// Detached executor registry
//
// Each detached script gets its OWN local SignalContext, parented to
// whatever ctx.sig was at the point of detach (root sig for a top-level
// detach, or an ancestor detach's own local_sig for a nested one). This is
// what makes a script individually stoppable: `signal <id> terminate` sets
// only that job's local flag, which isInterrupted()/isTerminated() check
// FIRST before ever consulting parent authority — so siblings and the root
// are untouched. Global signals (Ctrl+C, process shutdown) still reach every
// job regardless, via the same parent chain — nothing about targeted
// signaling weakens that.
//
// DetachedExecutor is heap-owned via unique_ptr specifically so its address
// (and therefore local_sig's address, and the address of the atomics
// local_sig points at) stays stable across executors_ vector growth. A
// SignalContext copied out of this object by value elsewhere would be fine
// too — its pointers still resolve to these stable atomics — but ctx.sig
// itself is always a pointer straight at this object, never a copy.
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
    // Set true by the running thread itself, right before it returns --
    // see the detach lambda below. joinable() alone can't answer "is this
    // job actually done" (see list()'s own comment), so all_finished()
    // checks this instead of inferring anything from the thread object.
    std::atomic<bool> finished{false};
    DetachedExecutor()                                    = default;
    DetachedExecutor(const DetachedExecutor&)             = delete;
    DetachedExecutor& operator=(const DetachedExecutor&)  = delete;
};

struct DetachedRegistry
{
    std::mutex                                    mutex_;
    std::vector<std::unique_ptr<DetachedExecutor>> executors_;
    std::atomic<uint64_t>                          next_id_{1};
    // Registers a new job slot and wires its local_sig's parent authority
    // to parent_sig BEFORE any thread starts — so the child's very first
    // isInterrupted() check already sees the correct chain. Returns a raw
    // pointer (stable for process lifetime) for the caller to hand to the
    // thread it's about to start via set_thread().
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
        // ACTIVE edge: the process root, ALWAYS -- never parent_sig, whatever
        // it happens to be. A detached job's lifetime is dictated by its own
        // internal state machine, not by the frame that launched it, so the
        // one thing its chain can safely terminate at is the one context
        // guaranteed to outlive every thread in the process
        // (RootSignalContext()'s function-local static).
        //
        // This is also a real dangling-pointer fix, not only a semantic one.
        // parent_sig is whatever ctx.sig was at the detach site, and for a
        // detach issued from inside a `run` that is &RunSignalScope::local_sig
        // -- a STACK local in execute_command's own CmdRun branch. `run` is
        // synchronous, so that frame returns as soon as the child script's
        // lines are exhausted, while anything it detached along the way keeps
        // running: every isInterrupted() from that thread afterward walked
        // `up` into a freed stack frame. SignalContext.h's own LIFETIME
        // comment already states this rule ("detach from a run scope roots at
        // global instead (CmdDetach, CommandExecutor.h)"); it was documented
        // and never implemented.
        exec->local_sig.setParent(&ETCS::RootSignalContext());

        // PASSIVE edge: the detaching parent, but ONLY when its lifetime is
        // structurally guaranteed -- which here means "it is one of THIS
        // registry's own executors", since those are unique_ptr-owned in
        // executors_ and live until join_all() at process shutdown. That is
        // exactly the detach->detach case, and preserving it means
        // `signal <parent_id> terminate` still cascades to nested detached
        // children rather than stopping one hop in.
        //
        // Left null for every other parent_sig -- a stack RunSignalScope
        // (unsafe, see above) or main's own WIRE_CONTEXT ctx (safe, but a
        // pure pass-through with no local flags of its own, so it carries
        // nothing worth reaching). The `up` edge already covers global
        // authority in both cases, so a null provider costs no reachability.
        //
        // Runs before push_back below, so exec itself is not yet in
        // executors_ and cannot match its own address.
        for (const auto& e : executors_)
        {
            if (&e->local_sig == parent_sig)
            {
                exec->local_sig.setProvider(parent_sig);
                break;
            }
        }
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
        for (auto& e : executors_)
            if (e->id == id) { e->local_terminate = 1; return true; }
        return false;
    }

    bool interrupt(uint64_t id)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& e : executors_)
            if (e->id == id) { e->local_interrupt = 1; return true; }
        return false;
    }
    // Snapshot of (id, script) for display. Doesn't attempt to report
    // liveness — joinable() isn't a trustworthy "still running" signal for
    // detached long-lived work, and removing finished entries safely would
    // require knowing a thread exited without racing join_all(). A job
    // stays listed until process shutdown joins everything.
    std::vector<std::pair<uint64_t, std::string>> list()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::pair<uint64_t, std::string>> out;
        for (auto& e : executors_)
            out.emplace_back(e->id, e->script);
        return out;
    }
    void join_all()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& e : executors_)
            if (e->thread.joinable()) e->thread.join();
        executors_.clear();
    }
    // True iff every registered executor has marked itself finished. An
    // empty registry (nothing ever detached) returns true trivially — see
    // wait_for_environment_drain's own comment for why that's correct.
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
// local_sig, minus the thread and minus registry bookkeeping (nothing
// external can target a `run` scope by id — it's gone by the time control
// returns to the parent line, so there's nothing to list or signal after
// the fact). Same reasoning as DetachedRegistry::create(): a `run`'d
// child gets its OWN atomics, parented to the calling script's own sig,
// so the child's local interrupt/terminate state can never alias or
// mutate the parent's — critical here specifically because `run` is
// synchronous and shares the calling thread, so there's no thread
// boundary to accidentally rely on for isolation the way detach has.
// Never copy/move: local_sig holds raw pointers into this object's own
// atomics, so its address must stay stable for its whole lifetime —
// keep instances on the stack in execute_command, never in a container.
// ---------------------------------------------------------------------------
struct RunSignalScope
{
    SignalFlag      local_interrupt{0};
    SignalFlag      local_terminate{0};
    SignalFlag      local_user1{0};
    SignalContext   local_sig;
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


inline ExecuteResult execute_command(const Command& cmd,
                                      ExecutionContext& ctx,
                                      const ExecSource& src)
{
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
        if constexpr (std::is_same_v<T, CmdPrintContext>)
        {
            ETCS_LOG("CommandExecutor", ctx.describe());
            if (!ctx.names.empty())
            {
                ETCS_LOG("CommandExecutor", "  names:");
                for (const auto& [name, rid] : ctx.names)
                    ETCS_LOG("CommandExecutor", "    " << name
                             << " -> RID:" << rid);
            }
            return {ExecuteStatus::Ok, ""};
        }
        if constexpr (std::is_same_v<T, CmdBind>)
        {
            if (!ctx.has_entity())
            {
                ETCS::exec_warn(src, "as: no entity in context to bind.");
                return {ExecuteStatus::Error, "No entity to bind."};
            }
            ctx.bind(c.name, ctx.active_rid);
            ETCS_LOG("CommandExecutor", "bound '" << c.name
                     << "' -> RID:" << ctx.active_rid);
            return {ExecuteStatus::Ok, ""};
        }
        if constexpr (std::is_same_v<T, CmdUnflag>)
        {
#ifdef ETCS_LOADER


            if (!ctx.has_entity())
            {
                ETCS::exec_warn(src, "unflag: no entity in context.");
                return {ExecuteStatus::Error, "No entity in context."};
            }
            ETCS::Entity* e = ETCS::get_entity_by_rid(ctx.module_name, ctx.tag_name,
                                                         ctx.active_rid, src);
            if (!e) return {ExecuteStatus::Error, "Entity not alive."};
            // removeTag routes through the SAME TagModifyEvent /
            // Scope::interruptOne path any other tag removal already
            // does (Entity.h) -- if c.flag names an active_scope_*
            // label (ScopeTag, Bundles.h) this is what actually reaches
            // in and interrupts that stream call's own SignalContext,
            // not merely a bookkeeping removal.
            try { e->removeTag(ETCS::Buffer(c.flag.c_str())); }
            catch (const std::exception& ex)
            {
                ETCS::exec_warn(src, std::string("unflag: ") + ex.what());
                return {ExecuteStatus::Error, ex.what()};
            }
            ETCS_LOG("CommandExecutor", "unflag: removed '" << c.flag << "' from "
                     << ctx.module_name << "::" << ctx.tag_name
                     << " RID:" << ctx.active_rid);
#endif
            return {ExecuteStatus::Ok, ""};
        }
        if constexpr (std::is_same_v<T, CmdKill>)
        {
#ifdef ETCS_LOADER
            if (!ctx.has_entity())
            {
                ETCS::exec_warn(src, "kill: no entity in context.");
                return {ExecuteStatus::Error, "No entity in context."};
            }
            ETCS::Entity* e = ETCS::get_entity_by_rid(ctx.module_name, ctx.tag_name,
                                                         ctx.active_rid, src);
            if (!e) return {ExecuteStatus::Error, "Entity not alive."};

            // Both forms only REQUEST -- the scope leaves the registry in
            // ~ScopeTag, when its body actually notices and returns. So
            // neither branch waits, and a script that needs the work to have
            // genuinely stopped has to observe that some other way. That is
            // the same contract every other interrupt in this system has;
            // making `kill` block would mean blocking the executor thread on
            // a body that may be mid-syscall.
            if (c.has_index)
            {
                if (!e->interruptScopeAt(c.label, c.index))
                {
                    ETCS::exec_warn(src, "kill: no live '" + c.label + "' at index "
                                      + std::to_string(c.index) + ".");
                    return {ExecuteStatus::Error, "No such scope."};
                }
                ETCS_LOG("CommandExecutor", "kill: interrupt requested for "
                         << c.label << " " << c.index << " on "
                         << ctx.module_name << "::" << ctx.tag_name
                         << " RID:" << ctx.active_rid);
            }
            else
            {
                size_t n = e->interruptAllOfLabel(c.label);
                if (n == 0)
                {
                    ETCS::exec_warn(src, "kill: no live '" + c.label + "' on this entity.");
                    return {ExecuteStatus::Error, "No such scope."};
                }
                ETCS_LOG("CommandExecutor", "kill: interrupt requested for all "
                         << n << " live '" << c.label << "' on "
                         << ctx.module_name << "::" << ctx.tag_name
                         << " RID:" << ctx.active_rid);
            }
#endif
            return {ExecuteStatus::Ok, ""};
        }

        if constexpr (std::is_same_v<T, CmdSetModule>)
        {
#ifdef ETCS_LOADER
            if (!ctx.root_entity)
                return {ExecuteStatus::Error,
                    "Internal error: current ExecutionContext has no root_entity set "
                    "-- a top-level entry point (script-mode main(), a detach/run child, "
                    "run_socket_repl) failed to wire one in before this ran."};
            if (!ETCS::resolve_module(c.module, src, ctx.root_entity))
                return {ExecuteStatus::Error, "Module not found: " + c.module};
#endif
            ctx.set_module(c.module);
            if (!c.pending_name.empty()) ctx.pending_name = c.pending_name;
            ETCS_LOG("CommandExecutor", "context -> " << ctx.describe());
            return {ExecuteStatus::Ok, ""};
        }
        if constexpr (std::is_same_v<T, CmdSetTag>)
        {
#ifdef ETCS_LOADER
            if (!ctx.root_entity)
                return {ExecuteStatus::Error,
                    "Internal error: current ExecutionContext has no root_entity set "
                    "-- a top-level entry point (script-mode main(), a detach/run child, "
                    "run_socket_repl) failed to wire one in before this ran."};
            if (!ETCS::resolve_module(c.module, src, ctx.root_entity))
                return {ExecuteStatus::Error, "Module not found: " + c.module};
            if (!ETCS::verify_tag(ctx.root_entity, c.module, c.tag, src))
                return {ExecuteStatus::Error, "Tag not found: " + c.tag};
#endif
            ctx.module_name = c.module;
            ctx.set_tag(c.tag);
            if (!c.pending_name.empty()) ctx.pending_name = c.pending_name;
            ETCS_LOG("CommandExecutor", "context -> " << ctx.describe());
            return {ExecuteStatus::Ok, ""};
        }

        if constexpr (std::is_same_v<T, CmdSetEntity>)
        {
            ETCS::RID rid = c.rid;
            if (rid == 0 && !c.name.empty())
            {
                rid = ctx.resolve_name(c.name);
                if (rid == 0)
                {
                    ctx.module_name  = c.module;
                    ctx.tag_name     = c.tag;
                    ctx.active_rid   = 0;
                    ctx.pending_name = c.name;
                    ETCS_LOG("CommandExecutor", "context -> " << ctx.describe());
                    return {ExecuteStatus::Ok, ""};
                }
            }
#ifdef ETCS_LOADER
            ETCS::Entity* e = ETCS::get_entity_by_rid(c.module, c.tag, rid, src);
            if (!e) return {ExecuteStatus::Error, "Entity not alive."};
#endif
            ctx.module_name  = c.module;
            ctx.tag_name     = c.tag;
            ctx.active_rid   = rid;
            ctx.pending_name.clear();
            ETCS_LOG("CommandExecutor", "context -> " << ctx.describe());
            return {ExecuteStatus::Ok, ""};
        }
        if constexpr (std::is_same_v<T, CmdSpawn>)
        {
            std::string module = c.module.empty() ? ctx.module_name : c.module;
            std::string tag    = c.tag.empty()    ? ctx.tag_name    : c.tag;
            if (module.empty() || tag.empty())
            {
                ETCS::exec_warn(src, "spawn: no module/tag in context. Use: spawn Module::Tag");
                return {ExecuteStatus::Error, "No context for spawn."};
            }
            if (!c.name.empty()) ctx.pending_name = c.name;

#ifdef ETCS_LOADER
            ETCS::Entity* e = ETCS::spawn_entity(module, tag, ctx, src, true);
            if (!e) return {ExecuteStatus::Error, "Spawn failed."};
            ETCS_LOG("CommandExecutor", "Spawned " << module << "::" << tag
                     << " RID:" << e->getRID());
            ETCS_LOG("CommandExecutor", "context -> " << ctx.describe());
#endif
            return {ExecuteStatus::Ok, ""};
        }
        if constexpr (std::is_same_v<T, CmdList>)
        {
            std::string module = c.module.empty() ? ctx.module_name : c.module;
            std::string tag    = c.tag.empty()    ? ctx.tag_name    : c.tag;
#ifdef ETCS_LOADER
            // `list` NARROWS as context narrows, rather than refusing until
            // it is fully specified. Nothing here is new information -- the
            // navigator already renders all three of these -- it is the same
            // three views reachable from the line surface, which is the one
            // scripts and sessions actually use.
            //
            // Refusing was the wrong default: at the point someone types
            // `list` with no context, "what is there" is precisely the
            // question being asked, and answering it with "specify what you
            // want listed" is the least useful possible response.
            //
            //   list                  -> live modules in this runtime
            //   list <Module>         -> that module's tags, with live counts
            //   list <Module>::<Tag>  -> the instances (unchanged)
            if (module.empty())
            {
                auto& registry = ETCS::EventNode::getInstance().stream.module_registry;
                std::vector<std::string> live;
                for (const auto& [name, mod_ptr] : registry)
                    if (mod_ptr) live.push_back(name);   // null value == vacant, not loaded
                std::sort(live.begin(), live.end());
                if (live.empty())
                {
                    ETCS_LOG("CommandExecutor", "no modules loaded in this runtime.");
                }
                else
                {
                    ETCS_LOG("CommandExecutor", "live modules [" << live.size() << "]");
                    for (const auto& name : live)
                        ETCS_LOG("CommandExecutor", "  " << name);
                }
                return {ExecuteStatus::Ok, ""};
            }
            if (tag.empty())
            {
                if (!ctx.root_entity)
                    return {ExecuteStatus::Error,
                        "Internal error: current ExecutionContext has no root_entity set."};
                // Resolving to read the tag list also LOADS the module if it
                // was not already anchored -- the same thing selecting it
                // with `context` does, and the same thing the navigator's own
                // ResolveEvent does before showing this exact view. Listing
                // what a module exports is not meaningfully lighter than
                // attaching to it.
                if (!ETCS::resolve_module(module, src, ctx.root_entity))
                    return {ExecuteStatus::Error, "Module not found: " + module};
                const auto& tags = ctx.root_entity.module().getTags();
                ETCS_LOG("CommandExecutor", module << " [" << tags.size() << " tags]");
                for (const auto& t : tags)
                {
                    const std::string tname = t.toString();
                    const ETCS::RIDListHandle* h = ETCS::get_handle(module, tname);
                    size_t n = h ? h->invoke_count() : 0;
                    ETCS_LOG("CommandExecutor", "  " << tname
                             << "  (" << n << " live)");
                }
                return {ExecuteStatus::Ok, ""};
            }
            const ETCS::RIDListHandle* handle = ETCS::get_handle(module, tag);
            if (!handle)
            {
                ETCS_LOG("CommandExecutor", module << "::" << tag
                         << " -- no instances (tag never loaded)");
                return {ExecuteStatus::Ok, ""};
            }
            std::vector<ETCS::RID> rids;
            handle->invoke_collect_rids(rids);
            ETCS_LOG("CommandExecutor", module << "::" << tag
                     << " [" << rids.size()
                     << " live]");
            for (size_t i = 0; i < rids.size(); ++i)
            {

                std::string alias;
                for (const auto& [n, r] : ctx.names)
                    if (r == rids[i]) { alias = " (" + n + ")"; break; }
                ETCS_LOG("CommandExecutor", "  [" << i << "] "
                         << "RID:" << rids[i] << alias
                         << (rids[i] == ctx.active_rid ? " *" : ""));
            }
#else
            // No registry to introspect without the loader, so the original
            // "say what you want listed" guard is still the only answer here.
            if (module.empty() || tag.empty())
            {
                ETCS::exec_warn(src, "list: no module/tag specified or in context.");
                return {ExecuteStatus::Error, "No context for list."};
            }
#endif
            return {ExecuteStatus::Ok, ""};
        }
        if constexpr (std::is_same_v<T, CmdSelect>)
        {
            if (!ctx.has_module() || !ctx.has_tag())
            {
                ETCS::exec_warn(src, "select: no module/tag in context.");
                return {ExecuteStatus::Error, "No context for select."};
            }
#ifdef ETCS_LOADER
            const ETCS::RIDListHandle* handle =
                ETCS::get_handle(ctx.module_name, ctx.tag_name);
            if (!handle)
            {
                ETCS::exec_warn(src, "No ridlist for current context.");
                return {ExecuteStatus::Error, "No ridlist."};
            }
            std::vector<ETCS::RID> rids;
            handle->invoke_collect_rids(rids);
            if (c.index >= rids.size())
            {
                ETCS::exec_warn(src, "Index " + std::to_string(c.index)
                                  + " out of range (" + std::to_string(rids.size()) + " live).");
                return {ExecuteStatus::Error, "Index out of range."};
            }

            ETCS::Entity* e = handle->invoke_get(rids[c.index]);
            if (!e)
            {
                ETCS::exec_warn(src, "Entity at index " + std::to_string(c.index)
                                  + " is no longer alive.");
                return {ExecuteStatus::Error, "Entity dead."};
            }
            ctx.set_entity(rids[c.index]);
            ctx.flush_pending_name(rids[c.index], false);
            ETCS_LOG("CommandExecutor", "context -> " << ctx.describe());
#endif
            return {ExecuteStatus::Ok, ""};
        }
        if constexpr (std::is_same_v<T, CmdDetach>)
        {
#ifdef ETCS_LOADER
            // Resolve all ORDINARY bindings from the parent name table now
            // — snapshot. "root" is deliberately NOT in child_names yet
            // (see resolve_run_bindings' own comment) — it's added below,
            // inside the detached thread, right after that thread
            // constructs its own fresh Root.
            std::unordered_map<std::string, ETCS::RID> child_names;
            if (!ETCS::resolve_run_bindings(c.bindings, ctx, src, child_names))
                return {ExecuteStatus::Error, "detach: binding resolution failed."};
            for (const auto& [k, v] : child_names)
                ETCS_LOG("CommandExecutor", "detach: injecting " << k
                         << " -> RID:" << v << " into child executor");

            // Resolve script path relative to the origin of the current
            // script, falling back to that script's own #IMPORT target
            // (if any) when the name isn't found locally.
            std::string script_path = ETCS::resolve_script_path(src.origin, c.script);
            // Own local SignalContext, parented to ctx.sig (root, or an
            // ancestor detach's local_sig for nested detach) — see
            // DetachedRegistry's comment above for why.
            DetachedExecutor* exec = DetachedRegistry::getInstance().create(c.script, ctx.sig);
            uint64_t exec_id = exec->id;
            ETCS_LOG("CommandExecutor", "detach: launching " << script_path
                     << " [id:" << exec_id << "]");
            // child_names is a snapshot; the executor's local_sig is already
            // parented to ctx.sig — interrupt/terminate propagate down that
            // chain, and can additionally be targeted individually via
            // `signal <exec_id> terminate` regardless of what's happening
            // elsewhere in the process.
            //
            // Deliberately NOT capturing ctx.root_entity for the child at
            // all. The old version of this lambda captured it by pointer
            // and handed it straight to child_ctx.root_entity -- meaning
            // every detached script sharing one parent (exactly
            // gdata_server.etcs's own shape: graphical_script.etcs and
            // simple_server.etcs both detached from the same top-level
            // script) fought over that ONE Root's single module_ slot.
            // attachModule's own already-bound guard silently drops any
            // second, DIFFERENT module request against an entity that
            // already has one bound -- so whichever detached script's
            // `context` line ran first "won" the shared root, and every
            // other detached sibling wanting a different module (here:
            // WindowProvider, NetworkProvider) got dropped outright, which
            // is exactly the failure this fixes. Each detached thread now
            // constructs its OWN fresh Root, scoped to exactly that
            // thread's own lifetime -- mirroring run_socket_repl's own
            // local_root exactly -- and binds "root" to THAT Root's own
            // RID, not the parent's (see resolve_run_bindings' own
            // comment for why the "root" entry isn't in child_names until
            // here). If the module a detached script resolves is ALREADY
            // anchored elsewhere (the common case: this is a proxy
            // attach, not a fresh dlopen), this costs nothing beyond one
            // small Root allocation (no arena, no dispatch surface --
            // see Root's own class comment, Entity.h, for what it no
            // longer carries); if it's the first entity/Root ever to
            // touch that module, this fresh Root becomes its
            // lifetime_owner exactly as any other first attach would.
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
                child_names["root"] = detached_root.getRID();
                ETCS_LOG("CommandExecutor", "detach: root for this thread -> RID:"
                         << detached_root.getRID());
                ExecutionContext child_ctx;
                child_ctx.sig         = &exec->local_sig;
                child_ctx.names       = child_names;
                child_ctx.root_entity = &detached_root;
                child_ctx.is_root     = false;  // children must not call join_all
                run_script(in, script_path, child_ctx);
                exec->finished.store(true, std::memory_order_release);
            });
            DetachedRegistry::getInstance().set_thread(exec, std::move(child_thread));
            ETCS_LOG("CommandExecutor", "detach: child executor launched for " << c.script
                     << " [id:" << exec_id << "]");
#endif
            return {ExecuteStatus::Ok, ""};
        }
        if constexpr (std::is_same_v<T, CmdRun>)
        {
#ifdef ETCS_LOADER
            std::unordered_map<std::string, ETCS::RID> child_names;
            if (!ETCS::resolve_run_bindings(c.bindings, ctx, src, child_names))
                return {ExecuteStatus::Error, "run: binding resolution failed."};
            std::string script_path = ETCS::resolve_script_path(src.origin, c.script);

            std::ifstream in(script_path);
            if (!in.is_open())
            {
                ETCS::exec_warn(src, "run: could not open '" + script_path + "'");
                return {ExecuteStatus::Error, "run: file not found."};
            }
            // Own local sig, parented to the caller's — never alias ctx.sig
            // directly. Same isolation DetachedExecutor gives detach, just
            // without a thread: this run's interrupt/terminate state can
            // never leak back into the parent script's own signal state.
            RunSignalScope run_scope(c.script, ctx.sig);
            // Own local Root, scoped to exactly this run's own lifetime --
            // deliberately NOT ctx.root_entity directly. Same reasoning as
            // detach's own fresh Root above: `run` shares the calling
            // thread rather than a separate one, so this isn't a
            // concurrent-access hazard the way detach's was, but it's the
            // same underlying bug in sequential form -- a nested run
            // wanting a DIFFERENT module than whatever the parent's own
            // root_entity already has bound would otherwise get silently
            // dropped by attachModule's already-bound guard. A run scope,
            // like a detach scope, gets to pick its own module context
            // freely; if the module it resolves is already anchored
            // elsewhere this is just an ordinary proxy attach, no
            // different from any other first touch of an already-loaded
            // module.
            ETCS::Root run_root(run_scope.local_sig);
            // "root" wasn't set by resolve_run_bindings above (see its own
            // comment) -- bind it now that run_root actually exists, so a
            // script referring to "root" by name inside this run scope
            // gets THIS Root's own RID, scoped to exactly this run's own
            // stack lifetime, not the caller's.
            child_names["root"] = run_root.getRID();

            ExecutionContext child_ctx;
            child_ctx.sig         = &run_scope.local_sig;
            child_ctx.names       = child_names;
            child_ctx.root_entity = &run_root;
            child_ctx.is_root     = false;
            ETCS_LOG("CommandExecutor", "run: " << script_path
                     << " (blocking)");
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
                // Child hit a plain 'exit' line — intentional, not a failure.
                ETCS_LOG("CommandExecutor", "  [run] " << c.script
                         << " stopped via 'exit' after " << elapsed);
            }

            else if (run_scope.local_sig.isInterrupted() || run_scope.local_sig.isTerminated())
            {
                // Fatal, but caused by a signal reaching this scope (set
                // locally, or inherited from ctx.sig's own parent chain —
                // e.g. a real Ctrl+C mid-run) rather than an actual crash.
                // The parent's own sig is untouched either way, since
                // local_sig owns its own atomics.
                ETCS_LOG("CommandExecutor", "  [run] " << c.script
                         << " interrupted after " << elapsed
                         << " (parent unaffected)");
            }
            else
            {
                // Genuine crash: Fatal status, no signal explains it.
                std::cerr << "  [run] " << c.script
                           << " crashed after " << elapsed << "\n";
                return {ExecuteStatus::Fatal, "run: child script hit a fatal error."};
            }
#endif
            return {ExecuteStatus::Ok, ""};
        }
        if constexpr (std::is_same_v<T, CmdJobs>)
        {
            auto jobs = DetachedRegistry::getInstance().list();
            if (jobs.empty())
            {
                ETCS_LOG("CommandExecutor", "(no detached scripts running)");
            }
            else
            {
                for (const auto& [id, script] : jobs)
                {
                    ETCS_LOG("CommandExecutor", "  [" << id << "] " << script);
                }
            }
            return {ExecuteStatus::Ok, ""};
        }

        if constexpr (std::is_same_v<T, CmdSignal>)
        {
            bool found = c.terminate ? DetachedRegistry::getInstance().terminate(c.id)
                                      : DetachedRegistry::getInstance().interrupt(c.id);
            if (!found)
            {
                ETCS::exec_warn(src, "signal: no job with id " + std::to_string(c.id));
                return {ExecuteStatus::Error, "no such job"};
            }
            ETCS_LOG("CommandExecutor", (c.terminate ? "Terminating" : "Interrupting")
                     << " job [" << c.id << "]");
            return {ExecuteStatus::Ok, ""};
        }
        if constexpr (std::is_same_v<T, CmdAttach>)
        {
#ifdef ETCS_LOADER
            if (!c.inner) return {ExecuteStatus::Error, "add: no inner declaration."};
            // (module, tag, name) from whichever declaration form the interior
            // parsed as -- both carry the same three facts.
            std::string mod, tag, name;
            if (const CmdSpawn* s = std::get_if<CmdSpawn>(&c.inner->cmd))
            { mod = s->module; tag = s->tag; name = s->name; }
            else if (const CmdSetTag* t = std::get_if<CmdSetTag>(&c.inner->cmd))
            { mod = t->module; tag = t->tag; name = t->pending_name; }
            if (mod.empty() || tag.empty())
                return {ExecuteStatus::Error,
                    "add: could not resolve a module and tag from the declaration -- "
                    "set a module in context first, or write it as Module::Tag."};
            ETCS::RID parent_rid = ctx.resolve_name(c.parent_name);
            if (parent_rid == 0)
            {
                ETCS::exec_warn(src, "add: '" + c.parent_name + "' is not bound.");
                return {ExecuteStatus::Error, "Unbound parent name."};
            }

            ETCS::Entity* parent = ETCS::resolve_entity_anywhere(parent_rid);
            if (!parent)
            {
                ETCS::exec_warn(src, "add: '" + c.parent_name + "' (RID:"
                                  + std::to_string(parent_rid) + ") is no longer alive.");
                return {ExecuteStatus::Error, "Parent not alive."};
            }
            // Idempotent re-run: a name already bound to a live child OF THIS
            // PARENT retargets instead of attaching a second one. The parent
            // check is cheap (one pointer compare on a strong reference we
            // already hold) rather than necessary -- since delete cascades to
            // children, a parent and its children retarget or respawn
            // together and cannot desynchronise.
            if (!name.empty())
            {
                auto existing = PersistentNames::getInstance().find(name, mod, tag);
                if (existing)
                {
                    ETCS::Entity* prior = ETCS::resolve_entity_anywhere(existing->rid);
                    if (prior && prior->getParent() == parent)
                    {
                        ctx.module_name = mod;
                        ctx.tag_name    = tag;
                        ctx.set_entity(prior->getRID());
                        ctx.bind(name, prior->getRID());
                        ETCS_LOG("CommandExecutor", "add: '" << name
                                 << "' is already a child of " << c.parent_name
                                 << " (RID:" << prior->getRID() << ") -- retargeting.");
                        return {ExecuteStatus::Ok, ""};
                    }
                }
            }
            if (!ETCS::resolve_module(mod, src, ctx.root_entity))
                return {ExecuteStatus::Error, "Module not found: " + mod};
            auto& registry = ETCS::EventNode::getInstance().stream.module_registry;
            auto reg_it = registry.find(mod);
            if (reg_it == registry.end() || !reg_it->second)
                return {ExecuteStatus::Error, "add: module '" + mod + "' is not anchored."};

            // Same tag -> module -> dlsym resolution `spawn` already uses,
            // reaching the typed-child factory instead of the standalone one.
            // Called on THIS thread, never the ordering thread: the export
            // blocks on an AddTagEvent internally, which the ordering thread
            // would have to service itself.
            void* addr = reg_it->second->getTagFunction(tag + "_MakeChild");
            if (!addr)
            {
                ETCS::exec_warn(src, "add: '" + tag + "' in " + mod
                                  + " exports no _MakeChild -- rebuild the module.");
                return {ExecuteStatus::Error, "No _MakeChild export."};
            }

            using MakeChildResolver = ETCS::MakeChildFunc (*)();
            ETCS::MakeChildFunc make_child = reinterpret_cast<MakeChildResolver>(addr)();
            ETCS::Entity* child = nullptr;
            try { child = make_child(parent); }
            catch (const std::exception& ex)
            {
                ETCS::exec_warn(src, std::string("add: ") + ex.what());
                return {ExecuteStatus::Error, ex.what()};
            }
            if (!child) return {ExecuteStatus::Error, "add: child construction failed."};
 
            // Context follows the new child, matching spawn_entity: the next
            // line configures what was just attached without re-navigating.
            ctx.module_name = mod;
            ctx.tag_name    = tag;
            ctx.set_entity(child->getRID());
            if (!name.empty()) ctx.bind(name, child->getRID());
            ETCS_LOG("CommandExecutor", "add: attached " << mod << "::" << tag
                     << " RID:" << child->getRID() << " to " << c.parent_name
                     << " (RID:" << parent_rid << ")"
                     << (name.empty() ? "" : " as '" + name + "'"));
            ETCS_LOG("CommandExecutor", "context -> " << ctx.describe());
#endif
            return {ExecuteStatus::Ok, ""};
        }
        if constexpr (std::is_same_v<T, CmdAction>)
        {
            if (c.module.empty() || c.tag.empty())
            {
                ETCS::exec_warn(src, "action: unresolved module/tag.");
                return {ExecuteStatus::Error, "No context for action."};
            }
#ifdef ETCS_LOADER
            // ── Inline stream path ────────────────────────────────────────────
            // "Tag.ProduceAction producer_name -> Tag.ConsumeAction consumer_name"
            // Both entity names are resolved from the local name table.
            //
            // The CONSUMER owns the frame -- DEFINE_STREAM_FUNC_CONSUME runs its
            // body inline on this thread while PRODUCE enqueues -- so the pair is
            // built on the consumer and the producer is handed in. That is also
            // what lets the two ends live on DIFFERENT entities, and therefore
            // different modules: the old same-entity form checked both tags on
            // the producer, so "LocalDatabase.RowProduce -> ForumNode.LoadRows"
            // could not be expressed at all.
            if (c.is_inline_stream)
            {
                ETCS::Entity* producer = ETCS::resolve_inline_producer(c, ctx, src);
                if (!producer)
                    return {ExecuteStatus::Error, "Inline stream: producer entity unavailable."};
                ETCS::Entity* consumer_entity = ETCS::resolve_inline_consumer(c, ctx, src);
                if (!consumer_entity)
                    return {ExecuteStatus::Error, "Inline stream: consumer entity unavailable."};
                ETCS::Buffer prod_buf, cons_buf, config;
                prod_buf.write((c.tag + "." + c.action).c_str());
                cons_buf.write((c.consumer_tag + "." + c.consumer_action).c_str());
                std::string effective_payload =
                    ETCS::strip_leading_name_token(c.payload, ctx, producer->getRID());
                if (!effective_payload.empty()) config.write(effective_payload.c_str());
                ETCS_LOG("CommandExecutor",
                         c.module << "::" << c.tag << "." << c.action
                         << (c.target_name.empty() ? "" : " (" + c.target_name + ")")
                         << " -> "
                         << c.target_module << "::" << c.consumer_tag << "." << c.consumer_action
                         << (c.consumer_name.empty() ? "" : " (" + c.consumer_name + ")"));
                try { consumer_entity->call(producer, prod_buf, cons_buf, config, *ctx.sig); }
                catch (const std::exception& ex)
                {
                    ETCS::exec_warn(src, std::string("Inline stream error: ") + ex.what());
                    return {ExecuteStatus::Error, ex.what()};
                }
                catch (...)
                {
                    ETCS::exec_warn(src, "Inline stream crashed (unknown exception).");
                    return {ExecuteStatus::Fatal, "Unknown stream exception."};
                }
                return {ExecuteStatus::Ok, ""};
            }
 
            // ── Pending stream consumer path ──────────────────────────────────
            // The two-line form: a produce action on one line, then a context
            // switch, then the consume action. THIS line is the consumer, so it
            // resolves its own entity from ctx and calls with the remembered
            // producer handed in -- see the inline path's comment for why the
            // consumer is the one making the call.
            if (ctx.pending_stream.has_value())
            {
                const PendingStream& ps = ctx.pending_stream.value();
                ETCS::Entity* producer = ETCS::get_entity_by_rid(
                    ps.module, ps.tag, ps.producer_rid, src);
                if (!producer)
                {
                    ctx.pending_stream.reset();
                    return {ExecuteStatus::Error, "Pending stream producer entity is no longer alive."};
                }
                if (ctx.module_name != c.module || ctx.tag_name != c.tag)
                {
                    ctx.module_name = c.module;
                    ctx.set_tag(c.tag);
                }
                ETCS::Entity* consumer = ETCS::get_or_spawn_entity(ctx, src);
                if (!consumer)
                {
                    ctx.pending_stream.reset();
                    return {ExecuteStatus::Error, "Pending stream consumer entity unavailable."};
                }
                ETCS::Buffer prod_buf, cons_buf, config;
                prod_buf.write((ps.tag + "." + ps.produce_action).c_str());
                cons_buf.write((c.tag + "." + c.action).c_str());
                if (!ps.payload.empty()) config.write(ps.payload.c_str());
                ETCS_LOG("CommandExecutor",
                         ps.module << "::" << ps.tag << "." << ps.produce_action
                         << " -> "
                         << c.module << "::" << c.tag << "." << c.action
                         << (ps.payload.empty() ? "" : " [" + ps.payload + "]"));
                ctx.pending_stream.reset();
                try { consumer->call(producer, prod_buf, cons_buf, config, *ctx.sig); }
                catch (const std::exception& ex)
                {
                    ETCS::exec_warn(src, std::string("Stream pair error: ") + ex.what());
                    return {ExecuteStatus::Error, ex.what()};
                }
                catch (...)
                {
                    ETCS::exec_warn(src, "Stream pair crashed (unknown exception).");
                    return {ExecuteStatus::Fatal, "Unknown stream exception."};
                }
                return {ExecuteStatus::Ok, ""};
            }
 
            // ── Normal action path ────────────────────────────────────────────
            if (!ctx.root_entity)
                return {ExecuteStatus::Error,
                    "Internal error: current ExecutionContext has no root_entity set "
                    "-- a top-level entry point (script-mode main(), a detach/run child, "
                    "run_socket_repl) failed to wire one in before this ran."};
            if (!ETCS::resolve_module(c.module, src, ctx.root_entity))
                return {ExecuteStatus::Error, "Module not found: " + c.module};
            ETCS::Entity* e = nullptr;
            if (c.fully_qualified && (c.module != ctx.module_name || c.tag != ctx.tag_name))
            {
                ExecutionContext tmp_ctx = ctx;
                tmp_ctx.module_name  = c.module;
                tmp_ctx.tag_name     = c.tag;
                tmp_ctx.active_rid   = 0;
                tmp_ctx.pending_name.clear();
                e = ETCS::get_or_spawn_entity(tmp_ctx, src);
            }
            else
            {
                if (ctx.module_name != c.module || ctx.tag_name != c.tag)
                {
                    ctx.module_name = c.module;
                    ctx.set_tag(c.tag);
                }
                e = ETCS::get_or_spawn_entity(ctx, src);
            }
 
            if (!e) return {ExecuteStatus::Error, "No entity available for action."};
            ETCS::Buffer act_buf;
            act_buf.write((c.tag + "." + c.action).c_str());
            const bool is_stream =
                ETCS::EventNode::getInstance().stream
                    .isTypedActionStream(c.module, c.tag + "." + c.action);
            ETCS_LOG("CommandExecutor", c.module << "::" << c.tag
                     << "." << c.action
                     << (c.payload.empty() && c.target_module.empty() ? ""
                         : !c.target_module.empty()
                             ? " -> " + c.target_module + "::" + c.target_tag
                             : " " + c.payload)
                     << " [stream:" << is_stream << "]");
            if (is_stream && !c.target_module.empty())
            {
                ETCS::Entity* target = ETCS::resolve_stream_target(c, ctx, src);
                if (!target) return {ExecuteStatus::Error, "Stream target unavailable."};
                const std::string consumer_action =
                    c.target_name.empty() ? c.action : c.target_name;
                ETCS::Buffer cons_buf, config;
                cons_buf.write((c.target_tag + "." + consumer_action).c_str());
                if (!c.payload.empty()) config.write(c.payload.c_str());
                // `e` produces and `target` consumes, so the call belongs to
                // target -- the names read backwards from the old form because
                // the frame is owned by the consuming end.
                try { target->call(e, act_buf, cons_buf, config, *ctx.sig); }
                catch (const std::exception& ex)
                {
                    ETCS::exec_warn(src, std::string("Cross-entity stream error: ") + ex.what());
                    return {ExecuteStatus::Error, ex.what()};
                }
                catch (...)
                {
                    ETCS::exec_warn(src, "Cross-entity stream crashed.");
                    return {ExecuteStatus::Fatal, "Unknown stream exception."};
                }
                return {ExecuteStatus::Ok, ""};
            }
 
            if (is_stream)
            {
                ctx.pending_stream = PendingStream{
                    ctx.active_rid, c.module, c.tag, c.action,
                    ETCS::strip_leading_name_token(c.payload, ctx, ctx.active_rid)
                };
                ETCS_LOG("CommandExecutor", "  [stream pending consumer...]");
                return {ExecuteStatus::Ok, ""};
            }
            // Non-stream action
            // Strip leading entity name token from payload if it resolves to the
            // active entity in the name table. The script syntax:
            //   Window.Run window 600 600 'title'
            // means "call Run on entity 'window' with payload '600 600 'title''"
            // The executor already resolved 'window' to select the entity above,
            // so the name token must not reach the work function's typed parser.
            try
            {
                // Same stripping strip_leading_name_token() applies everywhere
                // else now -- kept as one call here rather than a fourth copy
                // of the underlying logic.
                std::string effective_payload =
                    ETCS::strip_leading_name_token(c.payload, ctx, ctx.active_rid);
                // Entity references (@name) become RIDs here, after the leading
                // selector strip -- so a work function taking a <rid> argument
                // (ConnectionManager::RegisterConsumer, and every filter/route
                // registration after it) can be written in a script by name
                // rather than by a number copied out of a log.
                effective_payload = ETCS::substitute_name_tokens(effective_payload, ctx);
                ETCS_LOG("CommandExecutor", "[workFunc] preparing payload_buf...");
                ETCS::Buffer payload_buf;
                if (!effective_payload.empty()) payload_buf.write(effective_payload.c_str());
                e->call(act_buf, payload_buf, *ctx.sig);
                ETCS_LOG("CommandExecutor", "[workFunc]: " << payload_buf);
            }
 
            catch (const std::exception& ex)
            {
                const bool upper = !c.action.empty()
                    && std::isupper((unsigned char)c.action[0]);
                const std::string hint = upper
                    ? " (did you mean 'context " + c.action + "'?)"
                    : " ('" + c.action + "' is not a valid shell command)";
                ETCS::exec_warn(src, "unknown action '" + c.action
                                  + "' on " + c.module + "::" + c.tag + hint);
                return {ExecuteStatus::Error, ex.what()};
            }
            catch (...)
            {
                ETCS::exec_warn(src, "Action crashed (unknown exception).");
                return {ExecuteStatus::Fatal, "Unknown action exception."};
            }
#endif
            return {ExecuteStatus::Ok, ""};
        }
        return {ExecuteStatus::Error, "unhandled command type"};
    }, cmd);
}
 
inline bool run_script(std::istream& in,
                        const std::string& origin,
                        ExecutionContext& ctx,
                        ExecuteStatus* out_status)
{
    if (out_status) *out_status = ExecuteStatus::Ok;
    std::string line;
    size_t line_number = 0;
    bool first_line = true;
    while (std::getline(in, line))
    {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t s = line.find_first_not_of(" \t");
        if (s == std::string::npos) continue;
        if (line[s] == '#') continue;
        ExecSource src{origin, line_number};
        Command cmd = parse_line(line, ctx, first_line);
        first_line = false;
        ExecuteResult result = execute_command(cmd, ctx, src);
        
        if (result.status == ExecuteStatus::Exit
         || result.status == ExecuteStatus::Fatal)
        {
            if (out_status) *out_status = result.status;
            return false;
        }
    }
 
    if (ctx.pending_stream.has_value())
    {
        ExecSource src{origin, 0};
        ETCS::exec_warn(src, "script ended with unmatched stream produce: '"
                          + ctx.pending_stream->produce_action + "' -- no consumer line followed.");
        ctx.pending_stream.reset();
    }
 
    // NOTE: detached child threads (spawned via `detach`) are intentionally
    // NOT joined here. run_script returning only means THIS script's own
    // lines are exhausted — any threads it detached along the way continue
    // running independently in the background, exactly as `detach` implies.
    // The caller (REPL loop, or another script) should proceed immediately
    // rather than blocking on work this script explicitly asked to run
    // out-of-band. Detached threads are joined at actual process shutdown —
    // see shutdown_detached_executors() below, called from main() after the
    // interactive REPL loop is actually left.
    return true;
}
 
#ifdef __linux__
// ---------------------------------------------------------------------------
// run_socket_repl — line-oriented Command pipeline driver over a raw fd.
// The network counterpart to run_script's std::istream driver: same
// parse_line/execute_command path a local script or the Root> prompt
// already uses, fed one line at a time off a socket instead of a file.
//
// Runs on ONE dedicated OS thread for the session's entire life — NOT a
// ThreadPool task. A REPL session blocks indefinitely awaiting user input;
// parking that on the shared pool would starve capacity other work (HTTP
// connections, etc.) needs. Same reasoning CmdDetach already uses for
// long-running scripts.
//
// Registered in DetachedRegistry — the same registry `detach` uses. A REPL
// session IS, structurally, exactly a detached executor: its own local
// SignalContext parented to whatever ctx it was handed, visible in `jobs`,
// killable via `signal <id> terminate` from the LOCAL console. The
// registry's `script` field is just a display label here, not a real path.
//
// SO_RCVTIMEO makes the blocking recv() periodically return so the loop can
// re-check session_ctx.isInterrupted()/isTerminated() even with no data
// pending — without this, a quiet remote client (or `signal ... terminate`)
// would leave this thread parked indefinitely, which would also hang
// shutdown_detached_executors()'s join_all() at process exit.
//
// Output capture: see LogSinkGuard / log_sink in Buffer.h. Only
// synchronous output is captured — see that comment for the scope limit.
// ---------------------------------------------------------------------------
 
// Filled in by ShellREPL.h's shell_startup(); null in any build without the
// navigator compiled in. A session stays in bare line mode until someone
// asks for `shell`, because that mode is the compatible one -- a renewal
// hook piping ReloadCerts down this socket must not land in a menu waiting
// for a selection it has no way to answer.
inline void (*g_session_navigator)(int fd, SignalContext& sig) = nullptr;

inline void run_socket_repl(int fd, SignalContext& session_ctx)
{
    struct timeval tv { 0, 300000 }; // 300ms
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
 
    // A remote .etcs shell session is a genuinely independent execution
    // root — nothing upstream of this call has an ExecutionContext to
    // inherit root_entity from (it arrives via a raw accepted fd, not a
    // detach/run binding), so it gets its own fresh Root here, living for
    // exactly this session's own lifetime.
    ETCS::Root local_root(session_ctx);
 
    ExecutionContext ctx;
    ctx.sig         = &session_ctx;
    ctx.root_entity = &local_root;
    ctx.is_root     = false;
 
    auto send_all = [fd](const std::string& s) -> bool
    {
        size_t off = 0;
        while (off < s.size())
        {
            ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    };
 
    auto prompt_line = [&ctx]() -> std::string
    {
        return (ctx.has_module() ? ctx.describe() : std::string("Root")) + "> ";
    };
 
    if (!send_all("ETCS remote shell. 'exit' to disconnect.\n" + prompt_line()))
    {
        ::close(fd);
        return;
    }
 
    std::string accum;
    char buf[ETCS_NETWORK_MAX_HEADER_SIZE];
    size_t line_no = 0;
 
    while (true)
 
    {
        if (session_ctx.isInterrupted() || session_ctx.isTerminated()) break;
 
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n == 0) break;                                     // peer closed
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue; // timeout, re-check signals
            break;                                              // real socket error
        }
 
        accum.append(buf, static_cast<size_t>(n));
 
        // Bound growth against a client that never sends a newline —
        // same defensive posture PicoHTTPParser's accum_ overflow check
        // takes for the same class of problem.
        if (accum.size() > ETCS_NETWORK_MAX_HEADER_SIZE * 4)
        {
            send_all("Line too long, disconnecting.\n");
            ::close(fd);
            return;
        }
 
        size_t nl;
        while ((nl = accum.find('\n')) != std::string::npos)
        {
            std::string line = accum.substr(0, nl);
            accum.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
 
            size_t first_non_ws = line.find_first_not_of(" \t");
            if (first_non_ws == std::string::npos || line[first_non_ws] == '#')
            {
                if (!send_all(prompt_line())) { ::close(fd); return; }
                continue;
            }
 
            ++line_no;

            // Opt-in navigator. Everything else on this socket is the plain
            // parse_line/execute_command path, which is what scripts and
            // hooks need; this is the one command that hands the session to
            // the menu-driven surface instead.
            if (line == "shell")
            {
                if (!g_session_navigator)
                {
                    send_all("shell: no navigator in this build.\n");
                }
                else
                {
                    g_session_navigator(fd, session_ctx);
                    if (session_ctx.isInterrupted() || session_ctx.isTerminated())
                    { ::close(fd); return; }
                }
                if (!send_all(prompt_line())) { ::close(fd); return; }
                continue;
            }

            ExecSource src{"(remote)", line_no};
            Command cmd = parse_line(line, ctx, line_no == 1);
 
            std::ostringstream captured;
            ExecuteStatus status;
            {
                LogSinkGuard sink_guard(&captured);
                status = execute_command(cmd, ctx, src).status;
            }
 
            std::string out = captured.str();
            if (!out.empty() && !send_all(out)) { ::close(fd); return; }
 
            if (status == ExecuteStatus::Exit)
            {
                send_all("Goodbye.\n");
                ::close(fd);
                return;
            }
            if (status == ExecuteStatus::Fatal)
            {
                send_all("Fatal error, disconnecting.\n");
                ::close(fd);
                return;
            }
 
            if (!send_all(prompt_line())) { ::close(fd); return; }
        }
    }
 
    ::close(fd);
}

// ---------------------------------------------------------------------------
// run_control_listener — the drain-mode counterpart to repl_shell_loop's
// stdin. Binds a Unix domain socket and hands every accepted connection to
// run_socket_repl on its own thread.
//
// This is a RELOCATION of the input stream, not a new subsystem. An
// interactive build's stream is stdin; a headless build had none at all,
// because wait_for_environment_drain waits for work to FINISH rather than
// for input to arrive. Two consequences followed from that, and this fixes
// both at once:
//
//   - No way to reach a running runtime. Every control path (ReloadCerts,
//     list, kill, jobs) was reachable only from a terminal that headless
//     mode does not have.
//   - The process did not stay up. A server's traces complete -- detach the
//     lobbies, Start, return -- so all_finished() goes true, the drain
//     unblocks, and shutdown takes the still-serving runtime with it. An
//     accept loop is a stream that never completes, which is exactly what
//     kept the REPL build alive.
//
// Each session is registered in DetachedRegistry -- the same registry
// `detach` uses, with the same signal parenting -- because a session IS a
// detached executor at root scope. That is not an analogy: it gets its own
// Root and ExecutionContext inside run_socket_repl, shows up in `jobs`, and
// answers to `signal <id> terminate` like any other job. Nothing here
// special-cases sockets; the thread body is CmdDetach's lambda with a
// different entry point.
//
// AF_UNIX, never AF_INET. This channel executes arbitrary commands against
// the runtime, so its only authority boundary is filesystem permissions --
// 0600 below. That is the same authority a terminal on this machine already
// carried, which is what makes this a relocation rather than a new hole. A
// TCP listener would be a different authority entirely and must never be
// added here.
//
// SO_RCVTIMEO on the LISTENING fd makes accept() return EAGAIN periodically
// so the loop re-checks signals -- same reason run_socket_repl sets it on
// the session fd, and what lets shutdown_detached_executors() actually
// reach this thread.
inline void run_control_listener(const std::string& path, SignalContext& sig)
{
    // A socket file left by a previous run would make bind() fail with
    // EADDRINUSE even though nothing holds it -- unlink first, matching how
    // every other daemon handles its own stale endpoint.
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

    // Owner only. Set after bind, since the socket file does not exist
    // before it.
    if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) < 0)
    {
        std::cerr << "[CommandExecutor] control listener: chmod 0600 on '" << path
                  << "' failed: " << std::strerror(errno)
                  << " -- refusing to listen on a socket whose permissions "
                     "are unknown.\n";
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

    struct timeval tv { 0, 300000 }; // 300ms -- see this function's own comment
    ::setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ETCS_LOG("CommandExecutor", "control listener: accepting sessions on '"
             << path << "' (0600) -- 'jobs' lists them, 'signal <id> terminate' "
                "ends one.");

    uint64_t session_no = 0;
    while (!(sig.isInterrupted() || sig.isTerminated()))
    {
        int cfd = ::accept(lfd, nullptr, nullptr);
        if (cfd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;                       // timeout or signal -- re-check above
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
            // run_socket_repl closes cfd on every path out of itself.
            run_socket_repl(cfd, exec->local_sig);
            exec->finished.store(true, std::memory_order_release);
        });
        DetachedRegistry::getInstance().set_thread(exec, std::move(session_thread));
    }

    ETCS_LOG("CommandExecutor", "control listener: closing '" << path << "'.");
    ::close(lfd);
    ::unlink(path.c_str());
}
#endif // __linux__
 
// -----------------------------------------------------------------------
// wait_for_environment_drain — the non-interactive counterpart to
// repl_shell_loop, for a drain-mode (no ETCS_REPL_SHELL) build's main().
// Blocks until every detached executor has finished on its own, or an
// interrupt/terminate signal arrives on sig. An empty DetachedRegistry
// (nothing was ever spawned/detached) returns immediately — correct, not
// an error: a drain build with no work queued has nothing to wait for.
// -----------------------------------------------------------------------
inline void wait_for_environment_drain(SignalContext& sig)
{
    ETCS_LOG("CommandExecutor",
        "Environment established -- waiting for all detached executors "
        "to finish, or an interrupt/terminate signal.");
 
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
        "wait_for_environment_drain: all detached executors finished -- "
        "environment drained.");
}
 
// shutdown_detached_executors — signals termination to every executor
// sharing the process's root signal authority (the root REPL context AND
// every `detach`ed child, since CmdDetach hands each child a local_sig
// parented back to ctx.sig — see DetachedRegistry::create above) and blocks
// until every detached thread has actually exited and been joined.
//
// Call this exactly once, at real process shutdown (see ActiveLoader.cpp),
// AFTER the interactive REPL loop has been left — never from inside
// run_script itself. Calling it there was the original bug: it made a
// root script block on its own detached children finishing before ever
// reaching the interactive shell, defeating the entire point of `detach`.
//
// Writes g_sig_term directly rather than through any one ExecutionContext's
// ctx.sig — RootSignalContext()'s `terminate` slot is wired to this exact
// global (see WIRE_ROOT_SIGNAL_CONTEXT() in SignalContext.h), so every
// SignalContext anywhere in the process whose parent chain traces back to
// the root observes it via isTerminated(), the same path Ctrl+C already
// uses for g_sig_int.
 
inline void shutdown_detached_executors()
{
    g_sig_term = 1;
    DetachedRegistry::getInstance().join_all();
}
 
} // namespace ETCS
 
#endif // COMMAND_EXECUTOR_H__
 
