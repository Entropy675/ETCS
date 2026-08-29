#ifndef COMMAND_H__
#define COMMAND_H__

// Command.h - parsing only, no ETCS event calls
// Produces Command values from raw input lines.
// The executor (CommandExecutor.h) consumes these.

#include <string>
#include <variant>
#include <optional>
#include <sstream>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include "ETCS_API.h"
#include "Entity.h"
#include "DynamicLoader.h"

namespace ETCS {

struct PendingStream
{
    ETCS::RID   producer_rid = 0;
    std::string module;
    std::string tag;
    std::string produce_action;
    std::string payload;
};

// ---------------------------------------------------------------------------
// PersistentNames — process-lifetime name -> (RID, module, tag) registry,
// deliberately separate from ExecutionContext::names (which is scoped to
// exactly one execution and discarded the moment that script/detach/run
// ends). This is what lets a script re-run later -- a fresh
// ExecutionContext, a fresh Root, no memory of anything the FIRST run
// bound -- still recognize and retarget the SAME named entities the
// first run created, rather than spawning duplicates every single time.
//
// Global, flat, name-keyed -- same shape ExecutionContext::names already
// has (a name means one thing everywhere, matching how "root" is already
// a single reserved name rather than something scoped per module).
// module/tag are stored alongside the RID so a lookup can verify "does
// this name's existing binding actually fit what THIS script is asking
// for" before ever reusing it -- a name bound against one module/tag is
// never silently handed back to a DIFFERENT module/tag asking for the
// same string; see find()'s own comment.
//
// No entry is ever proactively removed when its entity dies -- find()
// callers already re-verify liveness (get_entity_by_rid) before ever
// reusing a match, so a stale entry is harmless: just a few accumulated
// bytes for a name string, bounded by how many DISTINCT names a person
// has ever actually typed, not by how many times a script has run. If
// this ever needs active pruning, the natural hook is EntityUnloadEvent's
// own processing -- not added here yet.
// ---------------------------------------------------------------------------
struct PersistentNameEntry
{
    ETCS::RID   rid = 0;
    std::string module;
    std::string tag;
};

struct PersistentNames
{
    std::mutex mutex_;
    std::unordered_map<std::string, PersistentNameEntry> entries_;

    void record(const std::string& name, ETCS::RID rid,
                const std::string& module, const std::string& tag)
    {
        if (name.empty() || module.empty() || tag.empty()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        entries_[name] = PersistentNameEntry{rid, module, tag};
    }

    // Returns the entry only if `name` is bound AND its recorded
    // module/tag match what's being asked for -- a name bound under a
    // different module/tag never matches here, so the caller falls
    // through to an ordinary fresh spawn rather than getting back
    // something that doesn't fit the script's own requirements.
    std::optional<PersistentNameEntry> find(const std::string& name,
                                             const std::string& module,
                                             const std::string& tag)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(name);
        if (it == entries_.end()) return std::nullopt;
        if (it->second.module != module || it->second.tag != tag) return std::nullopt;
        return it->second;
    }

    // For REPL/browsing visibility -- every persisted name currently
    // recorded under a given module (any tag), regardless of whether
    // it's still alive. The caller (repl_shell_tag_loop) is the one that
    // actually checks liveness before display, same as it already does
    // for ordinary RIDListHandle entries.
    std::vector<std::pair<std::string, PersistentNameEntry>> forModule(const std::string& module)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::pair<std::string, PersistentNameEntry>> out;
        for (const auto& [name, entry] : entries_)
            if (entry.module == module) out.emplace_back(name, entry);
        return out;
    }

    static PersistentNames& getInstance()
    {
        static PersistentNames instance;
        return instance;
    }
};

struct ExecutionContext
{
    std::string module_name;
    std::string tag_name;
    ETCS::RID   active_rid = 0;
    SignalContext* sig = nullptr;
    ETCS::LifetimeOwner root_entity;
    
    // The entity or Root serving as "root" for this execution — any
    // entity/Root works (see Entity::getRootAncestor()); being
    // specifically tagged Root::TAG only matters to attachModule's own
    // claim-and-hand-off logic, not to anything here. Inherited from a
    // parent script's own ExecutionContext for detach/run children (same
    // value, valid as long as whatever it points to is still alive -- see
    // CmdDetach/CmdRun, CommandExecutor.h, both of which now construct
    // their OWN fresh Root for the child rather than reusing the
    // parent's); a genuine top-level entry point (run_socket_repl, a
    // freshly started .etcs file) allocates its own fresh Root on its own
    // stack instead, since there's nothing to inherit from.
    //
    // LifetimeOwner, not a bare Entity* -- in current practice this is
    // always a Root (every actual call site in this codebase constructs
    // one), but the field's own contract has always been "any root-level
    // anchor," and LifetimeOwner is what lets that stay true without
    // requiring Root to masquerade as an Entity the way it used to.
    
    ExecutionContext(ETCS::LifetimeOwner root = {}, SignalContext* s = nullptr) : sig(s), root_entity(root) {};


    std::unordered_map<std::string, ETCS::RID> names;
    std::string pending_name;
    std::optional<PendingStream> pending_stream;

    // True for the root executor — owns DetachedRegistry::join_all() on exit.
    // Detached child executors set this false so they never join themselves.
    bool is_root = true;

    bool has_module()       const { return !module_name.empty(); }
    bool has_tag()          const { return !tag_name.empty(); }
    bool has_entity()       const { return active_rid != 0; }
    bool has_pending_name() const { return !pending_name.empty(); }

    // Also mirrors into PersistentNames -- see that struct's own comment
    // for why. This is the single chokepoint every existing bind path
    // (as, flush_pending_name, a direct call) already goes through, so
    // hooking it here covers all of them uniformly rather than needing a
    // second, separately-maintained call at each site. Only records when
    // module_name/tag_name are actually populated -- an empty pair means
    // whatever called bind() hasn't established real module/tag context
    // yet, and PersistentNames::record itself already no-ops on empty
    // strings regardless, so this is a documentation note more than a
    // second guard.
    void bind(const std::string& name, ETCS::RID rid)
    {
        names[name] = rid;
        PersistentNames::getInstance().record(name, rid, module_name, tag_name);
    }

    ETCS::RID resolve_name(const std::string& name) const
    {
        auto it = names.find(name);
        return it != names.end() ? it->second : 0;
    }

    bool has_name(const std::string& name) const { return names.count(name) > 0; }

    void flush_pending_name(ETCS::RID rid, bool overwrite = false)
    {
        if (pending_name.empty()) return;
        if (overwrite || !has_name(pending_name))
            bind(pending_name, rid);
        pending_name.clear();
    }

    std::string describe() const
    {
        if (!has_module()) return "(no context)";
        std::string s = module_name;
        if (has_tag()) { s += "::" + tag_name; }
        if (has_entity())
        {
            s += " RID:" + std::to_string(active_rid);
            for (const auto& [n, r] : names)
                if (r == active_rid) { s += " (" + n + ")"; break; }
        }
        if (!pending_name.empty())
            s += " [name pending: " + pending_name + "]";
        if (pending_stream.has_value())
            s += " [stream pending: " + pending_stream->produce_action + "]";
        return s;
    }

    void set_module(const std::string& m)
    {
        module_name = m;
        tag_name.clear();
        active_rid = 0;
        pending_name.clear();
    }

    void set_tag(const std::string& t)
    {
        tag_name = t;
        active_rid = 0;
    }

    void set_entity(ETCS::RID rid) { active_rid = rid; }

    void clear()
    {
        module_name.clear();
        tag_name.clear();
        active_rid = 0;
        pending_name.clear();
        pending_stream.reset();
    }
};

// Command types

struct CmdPrintContext {};

struct CmdSetModule { std::string module; std::string pending_name; };

struct CmdSetTag { std::string module; std::string tag; std::string pending_name; };

struct CmdSetEntity
{
    std::string module;
    std::string tag;
    ETCS::RID   rid;
    std::string name;
    bool        is_new;
};

struct CmdSpawn { std::string module; std::string tag; std::string name; };

struct CmdList { std::string module; std::string tag; };

struct CmdSelect { size_t index; };

struct CmdBind { std::string name; };

// unflag <name> — removes a flag from the entity currently in context,
// via Entity::removeTag(Buffer). Deliberately a top-level verb, not
// folded into CmdAction: it's not a dispatch call at all (no module
// action table involved), just a direct tag-list mutation, same
// category as `as` (CmdBind) rather than an ordinary action. Routing it
// through Entity::removeTag means it goes through the SAME
// TagModifyEvent / Scope::interruptOne path any other tag removal
// already does -- if `name` happens to be an active_scope_* label
// (see ScopeTag, Bundles.h), this is what actually reaches in and
// interrupts that stream call's own SignalContext, not merely removes
// bookkeeping.
struct CmdUnflag { std::string flag; };

// kill <label> [index] — interrupts in-flight work on the entity currently
// in context. Omitting the index interrupts EVERY live call carrying that
// label; supplying one targets a single call by its position among the live
// entries sharing that label, in creation order (see Scope, Bundles.h).
//
// The no-index form is exactly what removing the shared "active_scope_<label>"
// flag already does, and this is deliberately a second way to reach it: the
// flag is bookkeeping the shell can see, while `kill Listen` says what it
// means. A script author should never need to know the flag exists.
//
// Deliberately a top-level verb, like CmdUnflag rather than CmdAction: it
// dispatches nothing, consults no module action table, and touches only the
// entity's own scope registry. It is also the reason this belongs in the
// LANGUAGE and not only in the REPL -- a script that starts a long-running
// stream currently has no way to stop it short of destroying the entity that
// owns it.
//
// has_index rather than a sentinel value: 0 is a perfectly valid index (the
// oldest live call of that label), so it cannot double as "unspecified".
struct CmdKill
{
    std::string label;
    size_t      index     = 0;
    bool        has_index = false;
};

// <name>.add( <line> ) — attach a typed child to an already-bound entity.
//
// The parenthesised argument is a NESTING BOUNDARY, not punctuation: what's
// inside is a complete, ordinary .etcs line, parsed by the same parse_line
// everything else goes through. That is why there's no bespoke argument
// syntax to keep in sync -- whatever declares a type today declares one
// inside add() tomorrow, automatically.
//
// The interior is parsed with the TAG cleared from the surrounding context,
// deliberately. `FileHtmlPage tree` parses as a tag declaration only while no
// tag is set; with one already in context the same text parses as an ACTION
// named FileHtmlPage. Clearing it makes the interior read as the declaration
// it visibly is, independent of whatever line preceded it.
//
// Unambiguous against Tag.Action by the naming rules this runtime enforces at
// the marketplace boundary: `add` is lowercase and action names are TitleCase,
// so no action can be spelled `add`, and nothing else in the language uses
// parentheses at all. Same structural (not probabilistic) guarantee that makes
// back/up/kill safe as shell verbs.
//
// The receiver is a NAME, not a context reference -- the parent is stated at
// the call, so a script can attach to something bound many lines ago without
// re-navigating to it.
struct AttachedCommand;   // completed below Command -- recursive variant needs
                          // indirection, and shared_ptr keeps Command copyable
                          // (ShellREPL constructs Command{cmd} by value).

struct CmdAttach
{
    std::string parent_name;
    std::shared_ptr<AttachedCommand> inner;
};

struct CmdAction
{
    std::string module;
    std::string tag;
    std::string action;
    std::string payload;

    std::string target_module;
    std::string target_tag;
    ETCS::RID   target_rid  = 0;
    std::string target_name;

    bool fully_qualified = false;

    bool        is_inline_stream  = false;
    std::string consumer_action;
    std::string consumer_tag;
    std::string consumer_name;
};

struct CmdDetach
{
    std::string                                  script;
    std::vector<std::pair<std::string,std::string>> bindings;
};

struct CmdRun
{
    std::string                                     script;
    std::vector<std::pair<std::string,std::string>> bindings;
};

struct CmdJobs {};

struct CmdSignal
{
    uint64_t id;
    bool     terminate;
};

struct CmdExit {};

struct CmdError { std::string message; };


using Command = std::variant<
    CmdPrintContext,
    CmdSetModule,
    CmdSetTag,
    CmdSetEntity,
    CmdSpawn,
    CmdList,
    CmdSelect,
    CmdBind,
    CmdUnflag,
    CmdAttach,
    CmdAction,
    CmdKill,
    CmdDetach,
    CmdRun,
    CmdJobs,
    CmdSignal,
    CmdExit,
    CmdError
>;


// Completed here, now that Command is a complete type. Only ever constructed
// via make_shared at a point where this definition is visible, so the
// shared_ptr's deleter is captured correctly despite the forward declaration
// above.
struct AttachedCommand { Command cmd; };

namespace detail {

inline bool split_module_tag(const std::string& s,
                              std::string& module, std::string& tag)
{
    auto pos = s.find("::");
    if (pos == std::string::npos) return false;
    module = s.substr(0, pos);
    tag    = s.substr(pos + 2);
    return !module.empty() && !tag.empty();
}

inline bool split_qualified_action(const std::string& s,
                                    std::string& module,
                                    std::string& tag,
                                    std::string& action)
{
    auto dot = s.rfind('.');
    if (dot == std::string::npos) return false;
    std::string scope = s.substr(0, dot);
    action = s.substr(dot + 1);
    return split_module_tag(scope, module, tag) && !action.empty();
}

inline bool is_uppercase_start(const std::string& s)
{
    return !s.empty() && std::isupper((unsigned char)s[0]);
}

inline bool is_rid(const std::string& s, ETCS::RID& out)
{
    if (s.empty()) return false;
    try {
        size_t end;
        unsigned long long v = std::stoull(s, &end);
        if (end != s.size()) return false;
        out = static_cast<ETCS::RID>(v);
        return true;
    } catch (...) { return false; }
}

inline bool is_valid_local_name(const std::string& s)
{
    if (s.empty()) return false;
    // Reserved — always refers to whatever entity/Root is serving as
    // root for the current execution (see ExecutionContext::root_entity,
    // and Entity::getRootAncestor() for how deeper code reaches one
    // without any global lookup). Never user-bindable via `as`, `spawn
    // <name>`, or a detach/run k=v binding — it's injected automatically
    // into every detach/run child instead (see resolve_run_bindings,
    // CommandExecutor.h), the same way ctx/sig propagate through the
    // executor machinery without needing to be named explicitly every
    // time.
    if (s == "root") return false;
    if (s.find("::") != std::string::npos) return false;
    if (s.find('.')  != std::string::npos) return false;
    for (char c : s)
        if (!std::isalnum((unsigned char)c) && c != '_') return false;
    return true;
}

inline bool parse_stream_target(const std::string& payload,
                                 std::string& t_module, std::string& t_tag,
                                 ETCS::RID& t_rid, std::string& t_name)
{
    if (payload.empty()) return false;
    if (!is_uppercase_start(payload)) return false;
    if (payload.find("::") == std::string::npos) return false;

    size_t sp = payload.find_first_of(" \t");
    std::string scope    = (sp == std::string::npos) ? payload : payload.substr(0, sp);
    std::string id_token = (sp == std::string::npos) ? "" : payload.substr(sp + 1);
    size_t rs = id_token.find_first_not_of(" \t");
    if (rs != std::string::npos) id_token = id_token.substr(rs); else id_token.clear();

    if (!split_module_tag(scope, t_module, t_tag)) return false;

    t_rid  = 0;
    t_name.clear();
    if (!id_token.empty())
    {
        if (!is_rid(id_token, t_rid))
            t_name = id_token;
    }
    return true;
}

inline bool parse_inline_stream(const std::string& rest,
                                 const std::string& ambient_module,
                                 std::string& producer_name,
                                 std::string& cons_module,
                                 std::string& cons_tag,
                                 std::string& cons_action,
                                 std::string& consumer_name)
{
    const std::string arrow = " -> ";
    size_t arrow_pos = rest.find(arrow);
    if (arrow_pos == std::string::npos) return false;

    std::string lhs = rest.substr(0, arrow_pos);
    std::string rhs = rest.substr(arrow_pos + arrow.size());

    size_t ls = lhs.find_first_not_of(" \t");
    size_t le = lhs.find_last_not_of(" \t");
    producer_name = (ls == std::string::npos) ? "" : lhs.substr(ls, le - ls + 1);

    size_t rs = rhs.find_first_not_of(" \t");
    if (rs != std::string::npos) rhs = rhs.substr(rs); else rhs.clear();
    if (rhs.empty()) return false;

    size_t sp = rhs.find_first_of(" \t");
    std::string cons_verb    = (sp == std::string::npos) ? rhs : rhs.substr(0, sp);
    std::string cons_id_rest = (sp == std::string::npos) ? "" : rhs.substr(sp + 1);
    size_t ci = cons_id_rest.find_first_not_of(" \t");
    if (ci != std::string::npos) cons_id_rest = cons_id_rest.substr(ci); else cons_id_rest.clear();
    consumer_name = cons_id_rest;

    auto dot = cons_verb.rfind('.');
    if (dot == std::string::npos) return false;
    cons_action = cons_verb.substr(dot + 1);
    std::string scope = cons_verb.substr(0, dot);

    std::string m, t;
    if (split_module_tag(scope, m, t))
    {
        cons_module = m;
        cons_tag    = t;
    }
    else
    {
        cons_module = ambient_module;
        cons_tag    = scope;
    }

    return !cons_action.empty() && !cons_tag.empty() && !cons_module.empty();
}

} // namespace detail

inline Command parse_line(const std::string& raw,
                           const ExecutionContext& ctx,
                           bool is_first_line = false)
{
    size_t start = raw.find_first_not_of(" \t");
    if (start == std::string::npos) return CmdError{"empty line"};
    std::string line = raw.substr(start);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
        line.pop_back();

    // Checked BEFORE the verb split below -- that splits on whitespace, and
    // the interior line contains spaces. Falls through silently (rather than
    // erroring) when the text before ".add(" isn't a valid local name, so a
    // payload that happens to contain the sequence is left alone.
    {
        size_t add_pos = line.find(".add(");
        std::string receiver = (add_pos == std::string::npos) ? "" : line.substr(0, add_pos);
        if (add_pos != std::string::npos && detail::is_valid_local_name(receiver))
        {
            size_t open  = add_pos + 4;   // index of '(' within ".add("
            int    depth = 0;
            size_t close = std::string::npos;
            for (size_t i = open; i < line.size(); ++i)
            {
                if (line[i] == '(') ++depth;
                else if (line[i] == ')' && --depth == 0) { close = i; break; }
            }
            if (close == std::string::npos)
                return CmdError{"add: unbalanced parentheses in '" + line + "'"};
            for (size_t i = close + 1; i < line.size(); ++i)
                if (!std::isspace((unsigned char)line[i]))
                    return CmdError{"add: unexpected content after ')'"};

            std::string interior = line.substr(open + 1, close - open - 1);
            size_t is = interior.find_first_not_of(" \t");
            size_t ie = interior.find_last_not_of(" \t");
            if (is == std::string::npos)
                return CmdError{"add: expected a type declaration inside ()"};
            interior = interior.substr(is, ie - is + 1);

            // Tag cleared, entity cleared -- see CmdAttach's own comment for
            // why the interior must parse as a declaration regardless of what
            // context the surrounding script happens to be in.
            ExecutionContext inner_ctx = ctx;
            inner_ctx.tag_name.clear();
            inner_ctx.active_rid = 0;
            inner_ctx.pending_name.clear();
            Command inner = parse_line(interior, inner_ctx, false);

            if (std::holds_alternative<CmdError>(inner))
                return inner;   // propagate the interior parser's own message
            if (!std::holds_alternative<CmdSpawn>(inner)
                && !std::holds_alternative<CmdSetTag>(inner))
                return CmdError{"add: '" + interior + "' is not a type declaration -- "
                                "expected 'Tag name' or 'spawn Module::Tag name'"};

            CmdAttach cmd;
            cmd.parent_name = receiver;
            cmd.inner       = std::make_shared<AttachedCommand>(AttachedCommand{std::move(inner)});
            return cmd;
        }
    }

    size_t sp = line.find_first_of(" \t");
    std::string verb = (sp == std::string::npos) ? line : line.substr(0, sp);
    std::string rest = (sp == std::string::npos) ? "" : line.substr(sp + 1);
    size_t rs = rest.find_first_not_of(" \t");
    if (rs != std::string::npos) rest = rest.substr(rs);
    else rest.clear();

    if (verb == "exit" || verb == "quit") return CmdExit{};

    if (verb == "as")
    {
        if (rest.empty())
            return CmdError{"as: expected a name"};
        if (!detail::is_valid_local_name(rest))
            return CmdError{"as: '" + rest + "' is not a valid local name"};
        return CmdBind{rest};
    }

    if (verb == "unflag")
    {
        if (rest.empty())
            return CmdError{"unflag: expected a flag name"};
        return CmdUnflag{rest};
    }
    
    if (verb == "kill")
    {
        if (rest.empty())
            return CmdError{"kill: expected a work-function label, e.g. 'kill Listen' or 'kill Listen 1'"};

        size_t sp2 = rest.find_first_of(" \t");
        std::string label   = (sp2 == std::string::npos) ? rest : rest.substr(0, sp2);
        std::string idx_str = (sp2 == std::string::npos) ? "" : rest.substr(sp2 + 1);
        size_t rs2 = idx_str.find_first_not_of(" \t");
        if (rs2 != std::string::npos) idx_str = idx_str.substr(rs2); else idx_str.clear();

        CmdKill cmd;
        cmd.label = label;
        if (!idx_str.empty())
        {
            try {
                size_t end;
                unsigned long long v = std::stoull(idx_str, &end);
                if (end != idx_str.size()) throw std::invalid_argument("trailing");
                cmd.index     = static_cast<size_t>(v);
                cmd.has_index = true;
            } catch (...) {
                return CmdError{"kill: invalid index '" + idx_str + "'"};
            }
        }
        return cmd;
    }

    if (verb == "context")
    {
        if (rest.empty()) return CmdPrintContext{};

        size_t sp2 = rest.find_first_of(" \t");
        std::string scope  = (sp2 == std::string::npos) ? rest : rest.substr(0, sp2);
        std::string id_str = (sp2 == std::string::npos) ? "" : rest.substr(sp2 + 1);
        size_t rs2 = id_str.find_first_not_of(" \t");
        if (rs2 != std::string::npos) id_str = id_str.substr(rs2); else id_str.clear();

        std::string module, tag;
        if (detail::split_module_tag(scope, module, tag))
        {
            if (!id_str.empty())
            {
                ETCS::RID rid = 0;
                if (detail::is_rid(id_str, rid))
                    return CmdSetEntity{module, tag, rid, "", false};
                bool known = ctx.has_name(id_str);
                return CmdSetEntity{module, tag, 0, id_str, !known};
            }
            return CmdSetTag{module, tag, ""};
        }
        else
        {
            if (!detail::is_uppercase_start(scope))
                return CmdError{"context: expected Module or Module::Tag, got '" + scope + "'"};
            return CmdSetModule{scope, ""};
        }
    }

    if (verb == "spawn")
    {
        if (rest.empty())
            return CmdSpawn{ctx.module_name, ctx.tag_name, ""};

        if (rest.find("::") == std::string::npos && detail::is_valid_local_name(rest))
            return CmdSpawn{ctx.module_name, ctx.tag_name, rest};

        size_t sp2 = rest.find_first_of(" \t");
        std::string scope   = (sp2 == std::string::npos) ? rest : rest.substr(0, sp2);
        std::string name_tk = (sp2 == std::string::npos) ? "" : rest.substr(sp2 + 1);
        size_t rs2 = name_tk.find_first_not_of(" \t");
        if (rs2 != std::string::npos) name_tk = name_tk.substr(rs2); else name_tk.clear();

        std::string module, tag;
        if (!detail::split_module_tag(scope, module, tag))
            return CmdError{"spawn: expected Module::Tag [name], got '" + rest + "'"};

        if (!name_tk.empty() && !detail::is_valid_local_name(name_tk))
            return CmdError{"spawn: '" + name_tk + "' is not a valid local name"};

        return CmdSpawn{module, tag, name_tk};
    }

    if (verb == "list")
    {
        if (rest.empty()) return CmdList{ctx.module_name, ctx.tag_name};
        std::string module, tag;
        if (!detail::split_module_tag(rest, module, tag))
            return CmdError{"list: expected Module::Tag, got '" + rest + "'"};
        return CmdList{module, tag};
    }

    if (verb == "detach")
    {
        if (rest.empty())
            return CmdError{"detach: expected a script filename"};

        std::istringstream iss(rest);
        std::string script_tok;
        iss >> script_tok;

        CmdDetach cmd;
        cmd.script = script_tok;

        std::string token;
        while (iss >> token)
        {
            auto eq = token.find('=');
            if (eq == std::string::npos)
                return CmdError{"detach: invalid binding '" + token + "', expected name=name"};
            std::string child_name  = token.substr(0, eq);
            std::string parent_name = token.substr(eq + 1);
            if (!detail::is_valid_local_name(child_name))
                return CmdError{"detach: '" + child_name + "' is not a valid name"};
            if (!detail::is_valid_local_name(parent_name))
                return CmdError{"detach: '" + parent_name + "' is not a valid name"};
            cmd.bindings.emplace_back(child_name, parent_name);
        }
        return cmd;
    }
    
    if (verb == "run")
    {
        if (rest.empty())
            return CmdError{"run: expected a script filename"};

        std::istringstream iss(rest);
        std::string script_tok;
        iss >> script_tok;

        CmdRun cmd;
        cmd.script = script_tok;

        std::string token;
        while (iss >> token)
        {
            auto eq = token.find('=');
            if (eq == std::string::npos)
                return CmdError{"run: invalid binding '" + token + "', expected name=name"};
            std::string child_name  = token.substr(0, eq);
            std::string parent_name = token.substr(eq + 1);
            if (!detail::is_valid_local_name(child_name))
                return CmdError{"run: '" + child_name + "' is not a valid name"};
            if (!detail::is_valid_local_name(parent_name))
                return CmdError{"run: '" + parent_name + "' is not a valid name"};
            cmd.bindings.emplace_back(child_name, parent_name);
        }
        return cmd;
    }
    
    if (verb == "jobs") return CmdJobs{};

    if (verb == "signal")
    {
        if (rest.empty())
            return CmdError{"signal: expected an id, e.g. 'signal 3' or 'signal 3 interrupt'"};

        size_t sp2 = rest.find_first_of(" \t");
        std::string id_str = (sp2 == std::string::npos) ? rest : rest.substr(0, sp2);
        std::string mode    = (sp2 == std::string::npos) ? "" : rest.substr(sp2 + 1);
        size_t rs2 = mode.find_first_not_of(" \t");
        if (rs2 != std::string::npos) mode = mode.substr(rs2); else mode.clear();

        uint64_t id = 0;
        try {
            size_t end;
            id = std::stoull(id_str, &end);
            if (end != id_str.size()) throw std::invalid_argument("trailing");
        } catch (...) {
            return CmdError{"signal: invalid id '" + id_str + "'"};
        }

        bool term = true;
        if (!mode.empty())
        {
            if (mode == "interrupt" || mode == "int")           term = false;
            else if (mode == "terminate" || mode == "term")     term = true;
            else return CmdError{"signal: unknown mode '" + mode + "', expected 'terminate' or 'interrupt'"};
        }

        return CmdSignal{id, term};
    }

    {
        bool all_digits = !verb.empty();
        for (char c : verb) if (!std::isdigit((unsigned char)c)) { all_digits = false; break; }
        if (all_digits)
        {
            try { return CmdSelect{std::stoul(verb)}; }
            catch (...) { return CmdError{"invalid index: " + verb}; }
        }
    }

    {
        std::string module, tag, action;
        if (detail::split_qualified_action(verb, module, tag, action))
        {
            std::string prod_name, c_module, c_tag, c_action, c_name;
            if (detail::parse_inline_stream(rest, module,
                                            prod_name, c_module, c_tag, c_action, c_name))
            {
                CmdAction cmd;
                cmd.module           = module;
                cmd.tag              = tag;
                cmd.action           = action;
                cmd.target_name      = prod_name;
                cmd.target_module    = c_module;
                cmd.target_tag       = c_tag;
                cmd.fully_qualified  = true;
                cmd.is_inline_stream = true;
                cmd.consumer_action  = c_action;
                cmd.consumer_tag     = c_tag;
                cmd.consumer_name    = c_name;
                return cmd;
            }

            std::string t_module, t_tag, t_name;
            ETCS::RID t_rid = 0;
            if (detail::parse_stream_target(rest, t_module, t_tag, t_rid, t_name))
                return CmdAction{module, tag, action, "", t_module, t_tag, t_rid, t_name, true, false, "", "", ""};
            return CmdAction{module, tag, action, rest, "", "", 0, "", true, false, "", "", ""};
        }
    }

    if (detail::is_uppercase_start(verb))
    {
        auto dot = verb.find('.');
        if (dot != std::string::npos)
        {
            std::string tag    = verb.substr(0, dot);
            std::string action = verb.substr(dot + 1);
            if (!action.empty())
            {
                std::string module = ctx.module_name;
                if (module.empty())
                    return CmdError{"'" + verb + "': no module in context, use Module::Tag.Action"};

                std::string prod_name, c_module, c_tag, c_action, c_name;
                if (detail::parse_inline_stream(rest, module,
                                                prod_name, c_module, c_tag, c_action, c_name))
                {
                    CmdAction cmd;
                    cmd.module           = module;
                    cmd.tag              = tag;
                    cmd.action           = action;
                    cmd.target_name      = prod_name;
                    cmd.target_module    = c_module;
                    cmd.target_tag       = c_tag;
                    cmd.fully_qualified  = false;
                    cmd.is_inline_stream = true;
                    cmd.consumer_action  = c_action;
                    cmd.consumer_tag     = c_tag;
                    cmd.consumer_name    = c_name;
                    return cmd;
                }

                std::string t_module, t_tag, t_name;
                ETCS::RID t_rid = 0;
                if (detail::parse_stream_target(rest, t_module, t_tag, t_rid, t_name))
                    return CmdAction{module, tag, action, "", t_module, t_tag, t_rid, t_name, false, false, "", "", ""};
                return CmdAction{module, tag, action, rest, "", "", 0, "", false, false, "", "", ""};
            }
        }

        if (!ctx.has_module() || is_first_line)
        {
            std::string pname;
            if (!rest.empty() && detail::is_valid_local_name(rest))
                pname = rest;
            return CmdSetModule{verb, pname};
        }
        else if (!ctx.has_tag())
        {
            std::string pname;
            if (!rest.empty() && detail::is_valid_local_name(rest))
                pname = rest;
            return CmdSetTag{ctx.module_name, verb, pname};
        }
        else
        {
            std::string prod_name, c_module, c_tag, c_action, c_name;
            if (detail::parse_inline_stream(rest, ctx.module_name,
                                            prod_name, c_module, c_tag, c_action, c_name))
            {
                CmdAction cmd;
                cmd.module           = ctx.module_name;
                cmd.tag              = ctx.tag_name;
                cmd.action           = verb;
                cmd.target_name      = prod_name;
                cmd.target_module    = c_module;
                cmd.target_tag       = c_tag;
                cmd.fully_qualified  = false;
                cmd.is_inline_stream = true;
                cmd.consumer_action  = c_action;
                cmd.consumer_tag     = c_tag;
                cmd.consumer_name    = c_name;
                return cmd;
            }

            std::string t_module, t_tag, t_name;
            ETCS::RID t_rid = 0;
            if (detail::parse_stream_target(rest, t_module, t_tag, t_rid, t_name))
                return CmdAction{ctx.module_name, ctx.tag_name, verb, "", t_module, t_tag, t_rid, t_name, false, false, "", "", ""};
            return CmdAction{ctx.module_name, ctx.tag_name, verb, rest, "", "", 0, "", false, false, "", "", ""};
        }
    }

    if (ctx.has_module() && ctx.has_tag())
    {
        std::string prod_name, c_module, c_tag, c_action, c_name;
        if (detail::parse_inline_stream(rest, ctx.module_name,
                                        prod_name, c_module, c_tag, c_action, c_name))
        {
            CmdAction cmd;
            cmd.module           = ctx.module_name;
            cmd.tag              = ctx.tag_name;
            cmd.action           = verb;
            cmd.target_name      = prod_name;
            cmd.target_module    = c_module;
            cmd.target_tag       = c_tag;
            cmd.fully_qualified  = false;
            cmd.is_inline_stream = true;
            cmd.consumer_action  = c_action;
            cmd.consumer_tag     = c_tag;
            cmd.consumer_name    = c_name;
            return cmd;
        }

        std::string t_module, t_tag, t_name;
        ETCS::RID t_rid = 0;
        if (detail::parse_stream_target(rest, t_module, t_tag, t_rid, t_name))
            return CmdAction{ctx.module_name, ctx.tag_name, verb, "", t_module, t_tag, t_rid, t_name, false, false, "", "", ""};
        return CmdAction{ctx.module_name, ctx.tag_name, verb, rest, "", "", 0, "", false, false, "", "", ""};
    }

    return CmdError{"unknown command: '" + verb + "'"};
}

} // namespace ETCS

#endif // COMMAND_H__
