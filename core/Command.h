#ifndef COMMAND_H__
#define COMMAND_H__
// Command.h - parsing only, no ETCS event calls
// Produces Command values from raw input lines.
// The executor (CommandExecutor.h) consumes these.
//
// ---------------------------------------------------------------------------
// THE GRAMMAR, IN FULL
//
// Every line is one of exactly two shapes.
//
//   BARE      requires <name> [Tag, ...]      -- must already exist and qualify
//             spawn    Module::Tag <name>     -- always creates
//             attach   Module::Tag <name>     -- binds an existing one, or refuses
//             ensure   Module::Tag <name>     -- binds an existing one, or creates
//             detach   <script> [k=v ...]     -- launch and continue
//             run      <script> [k=v ...]     -- launch and wait
//             exit
//
//   DOTTED    <name>.Action(payload)          -- module action, TitleCase
//             <name>.spawn(Module::Tag <n>)   -- runtime operation, lowercase
//             <name>.attach(Module::Tag <n>)
//             <name>.ensure(Module::Tag <n>)
//             <name>.kill(<label> [index])
//             <name>.unflag(<flag>)
//             <a>.Produce(p) -> <b>.Consume(p)   -- stream, one line, both ends
//
// Nothing else is a line.
//
// NOTE THE SIGNATURE OF parse_line BELOW: it takes no ExecutionContext.
// That is the whole design stated as a type. Under the previous grammar the
// same text parsed as a tag declaration, an action, or a payload depending on
// what a previous line had left in context -- so the parser needed the
// context, and a line could not be read without replaying everything before
// it. Every one of those ambiguities is gone, so the parameter is gone with
// them. A line means one thing, forever, in isolation, and the parser is the
// proof.
// ---------------------------------------------------------------------------
#include <string>
#include <variant>
#include <optional>
#include <sstream>
#include <cstring>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "ETCS_API.h"
#include "Entity.h"
#include "DynamicLoader.h"

namespace ETCS {

// ---------------------------------------------------------------------------
// NameBinding — what a name means. A RID plus the Module::Tag it was acquired
// under.
//
// module/tag are carried alongside the RID rather than re-derived on every
// use because they are what an action line no longer states. Under the old
// grammar every action carried its own `Module::Tag.Action`, and the context
// carried a second copy; now `web.Start()` names only the receiver, so the
// pair has to come from the binding.
//
// For a name this script acquired itself (spawn/attach/ensure) the pair is
// known at acquisition. For a name INJECTED as a detach/run binding only the
// RID crosses the boundary -- module/tag are recovered once, at injection, by
// resolving the entity and reading Entity::getSourceModule()/getSourceTag().
// Left empty only if that entity is already dead, which the injection path
// treats as an unmet binding rather than a usable name.
// ---------------------------------------------------------------------------
struct NameBinding
{
    ETCS::RID   rid = 0;
    std::string module;
    std::string tag;
};

// ---------------------------------------------------------------------------
// GlobalNames — the names introduced by the ROOT script, and nothing else.
//
// This replaces PersistentNames, and occupies its structural slot (one
// process-level, name-keyed, mutex-guarded table) with materially different
// write rules -- which is the entire point, so the difference is worth
// stating precisely:
//
//   PersistentNames was written by EVERY bind() anywhere, at any depth, in
//   any script, and never cleared. That is what let a name introduced in one
//   script silently retarget a spawn in an unrelated one, and what let a
//   second run of the same file bind onto the first run's entities. Sharing
//   happened because two scripts happened to pick the same word.
//
//   GlobalNames is written ONLY by the root executor (ctx.is_root), only for
//   names the root script itself introduced, and is cleared when the root
//   script ends. A leaf script's names never appear here at all, so two
//   leaves picking the same word never see each other -- they see the root's,
//   or nothing.
//
// Reads are open to every script in the tree: that is the "global" half of
// the two-scope rule (local closure + root globals, no ancestor chain).
// A read is never a guarantee of liveness -- callers re-verify before use,
// same as before.
// ---------------------------------------------------------------------------
struct GlobalNames
{
    std::mutex mutex_;
    std::unordered_map<std::string, NameBinding> entries_;

    void record(const std::string& name, const NameBinding& b)
    {
        if (name.empty() || b.rid == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        entries_[name] = b;
    }

    std::optional<NameBinding> find(const std::string& name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(name);
        if (it == entries_.end()) return std::nullopt;
        return it->second;
    }

    // Cleared when the root script ends -- a global's lifetime is the root's
    // lifetime, which for the root script IS the runtime's lifetime. Exists
    // so a runtime that runs a root script, finishes, and runs another does
    // not leak the first one's names into the second.
    void clear()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

    std::vector<std::pair<std::string, NameBinding>> snapshot()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return {entries_.begin(), entries_.end()};
    }

    static GlobalNames& getInstance()
    {
        static GlobalNames instance;
        return instance;
    }
};

struct ExecutionContext
{
    SignalContext*      sig = nullptr;
    ETCS::LifetimeOwner root_entity;

    // The entity or Root serving as "root" for this execution — any
    // entity/Root works (see Entity::getRootAncestor()); being specifically
    // tagged Root::TAG only matters to attachModule's own claim-and-hand-off
    // logic. Each detach/run child constructs its OWN fresh Root rather than
    // inheriting the parent's (see CmdDetach/CmdRun, CommandExecutor.h); a
    // genuine top-level entry point allocates its own.
    ExecutionContext(ETCS::LifetimeOwner root = {}, SignalContext* s = nullptr)
        : sig(s), root_entity(root) {}

    // This script's own names: what it introduced, plus what it was passed.
    // NOT the globals -- those are consulted as a second, separate lookup
    // (see resolve/resolve_binding below), never merged in here, so a script
    // can always tell its own names from the root's.
    std::unordered_map<std::string, NameBinding> names;

    // ---------------------------------------------------------------------
    // The liveness watch set — every RID this script spawned, attached,
    // ensured, or was passed. "Its closure," in the language doc's terms.
    //
    // Provenance, not a sweep: a RID that vanishes and is never named again
    // is not this script's problem, and a script that deletes its own entity
    // and moves on is not in error. What is in error is NAMING a receiver
    // this script depends on, after that entity has stopped resolving --
    // checked at the reference, which under mandatory receivers is exactly
    // one place per line.
    // ---------------------------------------------------------------------
    std::unordered_set<ETCS::RID> owned_;

    // Set by the executor when an owned RID fails to resolve at a reference.
    // run_script checks it after each line and stops with Vanished.
    ETCS::RID lost_rid = 0;

    // True for the root executor — owns DetachedRegistry::join_all() on exit,
    // and owns GlobalNames: only a root executor publishes into it, and only
    // a root executor clears it.
    bool is_root = true;

    void own(ETCS::RID rid)        { if (rid) owned_.insert(rid); }
    bool owns(ETCS::RID rid) const { return owned_.count(rid) > 0; }
    void note_lost(ETCS::RID rid)  { lost_rid = rid; }

    // Introduce a name. Publishes to GlobalNames only from the root script --
    // see that struct's comment for why that restriction is the whole
    // difference between this and the name table it replaces.
    void bind(const std::string& name, const NameBinding& b)
    {
        names[name] = b;
        own(b.rid);
        if (is_root) GlobalNames::getInstance().record(name, b);
    }

    void bind(const std::string& name, ETCS::RID rid,
              const std::string& module, const std::string& tag)
    {
        bind(name, NameBinding{rid, module, tag});
    }

    bool introduced(const std::string& name) const { return names.count(name) > 0; }

    // Local first, then globals. The two-scope rule, in one function: a
    // script's own names shadow the root's, and there is nothing in between
    // -- no parent chain, no ancestor walk, no depth-dependent resolution.
    std::optional<NameBinding> lookup(const std::string& name) const
    {
        auto it = names.find(name);
        if (it != names.end()) return it->second;
        return GlobalNames::getInstance().find(name);
    }

    ETCS::RID resolve_name(const std::string& name) const
    {
        auto b = lookup(name);
        return b ? b->rid : 0;
    }

    bool has_name(const std::string& name) const { return lookup(name).has_value(); }

    std::string describe() const
    {
        std::string s = "names[" + std::to_string(names.size()) + "]";
        for (const auto& [n, b] : names)
        {
            s += " " + n + "->RID:" + std::to_string(b.rid);
            if (!b.tag.empty()) s += "(" + b.module + "::" + b.tag + ")";
        }
        return s;
    }
};

// ---------------------------------------------------------------------------
// Command types
// ---------------------------------------------------------------------------

// The three script-resolved acquisition verbs. One struct rather than three,
// because they carry identical fields and differ in exactly one thing: what
// happens when the closure does not already hold that name. Splitting them
// into three types would duplicate the struct, the parse, and the whole
// executor branch to express a difference that is one enum wide.
//
//   Spawn   -- always creates. Never looks at the closure at all.
//   Attach  -- binds the closure's entity. Refuses if there isn't one.
//   Ensure  -- binds the closure's entity, or creates one.
//
// `requires` is deliberately NOT in this enum: it is not script-resolved. It
// names something the caller must already have supplied (or that the root
// published as a global), it takes no Module::Tag, and it can carry a tag
// constraint none of these three can. Different inputs, different outputs,
// its own command type.
enum class AcquireVerb { Spawn, Attach, Ensure };

inline const char* acquire_verb_name(AcquireVerb v)
{
    switch (v)
    {
        case AcquireVerb::Spawn:  return "spawn";
        case AcquireVerb::Attach: return "attach";
        case AcquireVerb::Ensure: return "ensure";
    }
    return "?";
}

// requires <name> [Tag1, Tag2, ...]
//
// No Module::Tag, ever -- see the language document's own reasoning. The
// optional bracket constrains by SLOT rather than type: every tag listed must
// be present on whatever entity gets bound.
//
// Two kinds of tag can appear in that list and both are checked identically
// here (Entity::hasTag routes both to the same map):
//
//   Bare        Deletable, Gate, Parser -- the is-a markers ETCS_MAKE_INSTANCE's
//               generated constructor adds via addTypeTag, fixed at
//               construction, never reachable by anything a script can do.
//   Origin-affixed  NetworkProvider::TLSContext -- the record that this entity
//               has had a child of that type spawned or attached onto it. A
//               fact about this entity's own causal history, not about its
//               type: two entities of the same concrete type can differ here.
//
// That difference is invisible to the check and decisive for its TIMING --
// bare tags are knowable from the resolved type at load, origin-affixed ones
// only once execution has actually reached this script. See the executor.
//
// `unflag` cannot reach either: it operates on the lowercase flags_ map,
// which is a different set entirely (Entity::hasTag routes on case).
struct CmdRequires
{
    std::string              name;
    std::vector<std::string> tags;   // empty == any live entity qualifies
};

// spawn/attach/ensure Module::Tag <name>
struct CmdAcquire
{
    AcquireVerb verb;
    std::string module;
    std::string tag;
    std::string name;
};

// <parent>.spawn/attach/ensure( Module::Tag <name> )
//
// Replaces `.add()` entirely. `.add()` could only ever create, so a script
// wanting to reuse a parent's existing child had no way to say so and built a
// second one -- which is exactly the duplicate-`tree` bug the strict-form
// transform surfaced across three files.
//
// The parent is part of the match for Attach/Ensure, not merely the search
// order: a child of some OTHER parent carrying the same name never binds
// here.
struct CmdChildAcquire
{
    AcquireVerb verb;
    std::string parent_name;
    std::string module;
    std::string tag;
    std::string name;
};

// <name>.Action(payload)
// <a>.Produce(payload) -> <b>.Consume(payload)
//
// No module, no tag, no fully_qualified flag, no target_rid. The receiver's
// name is the address, and its Module::Tag comes from the binding -- so the
// eight fields the old CmdAction needed to describe "which entity, maybe, and
// how to find it if not" collapse into one string.
struct CmdAction
{
    std::string receiver;
    std::string action;
    std::string payload;

    bool        is_stream = false;
    std::string consumer_receiver;
    std::string consumer_action;
    std::string consumer_payload;
};

// <name>.kill(<label> [index])
//
// Interrupts in-flight work on the named entity. Omitting the index
// interrupts EVERY live call carrying that label; supplying one targets a
// single call by position among the live entries sharing that label, in
// creation order (see Scope, Bundles.h).
//
// has_index rather than a sentinel: 0 is a valid index (the oldest live call
// of that label), so it cannot double as "unspecified".
struct CmdKill
{
    std::string receiver;
    std::string label;
    size_t      index     = 0;
    bool        has_index = false;
};

// <name>.unflag(<flag>)
//
// Removes a FLAG -- the freely-mutable lowercase set. Routed through
// Entity::removeTag, which means it goes through the same TagModifyEvent /
// Scope::interruptOne path any other flag removal does: if `flag` names an
// active_scope_* label (ScopeTag, Bundles.h), this reaches in and interrupts
// that stream call's own SignalContext rather than merely removing
// bookkeeping.
//
// It cannot touch the upper-case tag set a `requires` bracket tests. That is
// enforced one level down by Entity::hasTag/removeTag's own case routing, not
// here -- but it is the reason a bracketed `requires` is a stable assertion
// rather than something a later line could quietly invalidate.
struct CmdUnflag
{
    std::string receiver;
    std::string flag;
};

struct CmdDetach
{
    std::string                                     script;
    std::vector<std::pair<std::string,std::string>> bindings;
};

struct CmdRun
{
    std::string                                     script;
    std::vector<std::pair<std::string,std::string>> bindings;
};

struct CmdExit {};
struct CmdError { std::string message; };

using Command = std::variant<
    CmdRequires,
    CmdAcquire,
    CmdChildAcquire,
    CmdAction,
    CmdKill,
    CmdUnflag,
    CmdDetach,
    CmdRun,
    CmdExit,
    CmdError
>;

namespace detail {

inline std::string trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

inline bool is_uppercase_start(const std::string& s)
{
    return !s.empty() && std::isupper((unsigned char)s[0]);
}

inline bool is_lowercase_start(const std::string& s)
{
    return !s.empty() && std::islower((unsigned char)s[0]);
}

inline bool split_module_tag(const std::string& s,
                             std::string& module, std::string& tag)
{
    auto pos = s.find("::");
    if (pos == std::string::npos) return false;
    module = s.substr(0, pos);
    tag    = s.substr(pos + 2);
    return !module.empty() && !tag.empty()
        && is_uppercase_start(module) && is_uppercase_start(tag)
        && tag.find("::") == std::string::npos;
}

inline bool is_valid_local_name(const std::string& s)
{
    if (s.empty()) return false;
    // Reserved — always refers to whatever entity/Root is serving as root for
    // the current execution (see ExecutionContext::root_entity, and
    // Entity::getRootAncestor()). Never user-bindable; injected automatically
    // into every detach/run child (see resolve_run_bindings,
    // CommandExecutor.h).
    if (s == "root") return false;
    for (char c : s)
        if (!std::isalnum((unsigned char)c) && c != '_') return false;
    return true;
}

// A tag as it may appear inside a `requires` bracket: either a bare is-a
// marker (Gate) or an origin-affixed one (NetworkProvider::TLSContext). Both
// are upper-case leading, which is what separates them from the lowercase
// flag namespace `unflag` operates on.
inline bool is_valid_requires_tag(const std::string& s)
{
    if (!is_uppercase_start(s)) return false;
    auto sep = s.find("::");
    if (sep == std::string::npos)
    {
        for (char c : s)
            if (!std::isalnum((unsigned char)c) && c != '_') return false;
        return true;
    }
    std::string m, t;
    return split_module_tag(s, m, t);
}

// -------------------------------------------------------------------------
// find_closing_bracket — the matching ')' for the '(' at `open`, by depth
// count, QUOTE-AWARE.
//
// Payloads nest (SQL is full of parentheses) and payloads quote. A depth
// counter alone breaks on `ExecuteRaw(SELECT ')' FROM x)`; a quote check
// alone breaks on nested calls. Both together handle every payload this
// language can carry, and a payload with unbalanced parens OUTSIDE quotes has
// to be quoted -- there is no escape character, deliberately, because one
// would put a second parsing rule inside a span whose whole contract is that
// it is handed to the work function untouched.
// -------------------------------------------------------------------------
inline size_t find_closing_bracket(const std::string& s, size_t open)
{
    int  depth     = 0;
    bool in_single = false, in_double = false;
    for (size_t i = open; i < s.size(); ++i)
    {
        char ch = s[i];
        if (ch == '\'' && !in_double) { in_single = !in_single; continue; }
        if (ch == '"'  && !in_single) { in_double = !in_double; continue; }
        if (in_single || in_double) continue;
        if (ch == '(') ++depth;
        else if (ch == ')' && --depth == 0) return i;
    }
    return std::string::npos;
}

// The stream arrow, at bracket depth 0 and outside quotes. An `->` inside a
// payload (a SQL string, a path) is payload text and must not split the line.
inline size_t find_stream_arrow(const std::string& s)
{
    int  depth     = 0;
    bool in_single = false, in_double = false;
    for (size_t i = 0; i + 1 < s.size(); ++i)
    {
        char ch = s[i];
        if (ch == '\'' && !in_double) { in_single = !in_single; continue; }
        if (ch == '"'  && !in_single) { in_double = !in_double; continue; }
        if (in_single || in_double) continue;
        if (ch == '(') { ++depth; continue; }
        if (ch == ')') { --depth; continue; }
        if (depth == 0 && ch == '-' && s[i + 1] == '>') return i;
    }
    return std::string::npos;
}

// One dotted call: `<receiver>.<member>( <payload> )`.
struct DottedCall
{
    std::string receiver;
    std::string member;
    std::string payload;
};

inline bool parse_dotted(const std::string& s, DottedCall& out, std::string& err)
{
    size_t open = s.find('(');
    if (open == std::string::npos)
    {
        err = "expected '(' -- every action takes a bracketed argument list, "
              "'()' when it takes none";
        return false;
    }
    size_t dot = s.find('.');
    if (dot == std::string::npos || dot > open)
    {
        err = "expected <name>.Action(...) -- every action names the entity it "
              "acts on";
        return false;
    }

    out.receiver = trim(s.substr(0, dot));
    out.member   = trim(s.substr(dot + 1, open - dot - 1));

    if (!is_valid_local_name(out.receiver))
    {
        err = "'" + out.receiver + "' is not a valid name";
        return false;
    }
    if (out.member.empty())
    {
        err = "expected an action name between '.' and '('";
        return false;
    }
    for (char c : out.member)
        if (!std::isalnum((unsigned char)c) && c != '_')
        {
            err = "'" + out.member + "' is not a valid action name";
            return false;
        }

    size_t close = find_closing_bracket(s, open);
    if (close == std::string::npos)
    {
        err = "unbalanced parentheses -- quote a payload carrying unmatched "
              "brackets; a line never continues onto the next";
        return false;
    }
    for (size_t i = close + 1; i < s.size(); ++i)
        if (!std::isspace((unsigned char)s[i]))
        {
            err = "unexpected content after ')'";
            return false;
        }

    out.payload = trim(s.substr(open + 1, close - open - 1));
    return true;
}

// The interior of a receiver-scoped spawn/attach/ensure: `Module::Tag <name>`.
inline bool parse_typed_declaration(const std::string& s,
                                    std::string& module, std::string& tag,
                                    std::string& name, std::string& err)
{
    size_t sp = s.find_first_of(" \t");
    if (sp == std::string::npos)
    {
        err = "expected 'Module::Tag <name>' -- every acquisition names a name";
        return false;
    }
    std::string scope = trim(s.substr(0, sp));
    name              = trim(s.substr(sp + 1));

    if (!split_module_tag(scope, module, tag))
    {
        err = "expected Module::Tag, got '" + scope + "'";
        return false;
    }
    if (!is_valid_local_name(name))
    {
        err = "'" + name + "' is not a valid name";
        return false;
    }
    return true;
}

// k=v bindings shared by detach and run.
inline bool parse_bindings(std::istringstream& iss, const char* verb,
                           std::vector<std::pair<std::string,std::string>>& out,
                           std::string& err)
{
    std::string token;
    while (iss >> token)
    {
        auto eq = token.find('=');
        if (eq == std::string::npos)
        {
            err = std::string(verb) + ": invalid binding '" + token
                + "', expected name=name";
            return false;
        }
        std::string child_name  = token.substr(0, eq);
        std::string parent_name = token.substr(eq + 1);
        if (!is_valid_local_name(child_name))
        {
            err = std::string(verb) + ": '" + child_name + "' is not a valid name";
            return false;
        }
        if (!is_valid_local_name(parent_name))
        {
            err = std::string(verb) + ": '" + parent_name + "' is not a valid name";
            return false;
        }
        out.emplace_back(child_name, parent_name);
    }
    return true;
}

// Removed constructs, reported by name rather than as "unknown command".
// Every one of these was valid in the previous grammar and appears in scripts
// still on disk, so the migration deserves a sentence rather than a shrug.
inline std::optional<std::string> removed_construct(const std::string& verb)
{
    if (verb == "context")
        return "the ambient context is gone -- every line names its own "
               "receiver. Use 'spawn/attach/ensure Module::Tag <name>' to "
               "introduce a name, then '<name>.Action(...)'.";
    if (verb == "as")
        return "'as' is gone -- names come from requires/spawn/attach/ensure "
               "only, each of which names its name on the line that acquires it.";
    if (verb == "list" || verb == "select")
        return "'" + verb + "' is a browse operation, not a script line. The "
               "navigator lists and selects; a script names what it means.";
    if (verb == "jobs" || verb == "signal")
        return "'" + verb + "' is a control operation on the running runtime, "
               "reachable from a session on the control socket -- not a line in a "
               "trace. A script launches work with detach/run; it does not "
               "administer it afterward.";
    return std::nullopt;
}

} // namespace detail

// ---------------------------------------------------------------------------
// parse_line — text to Command. No context, by design (see the header note).
// ---------------------------------------------------------------------------
inline Command parse_line(const std::string& raw)
{
    std::string line = detail::trim(raw);
    if (line.empty()) return CmdError{"empty line"};

    // ---- Stream: two dotted calls, one line, arrow between them ----------
    //
    // Checked before the verb split, since both halves contain dots, brackets
    // and spaces. There is no two-line form: a produce whose consumer is on
    // the next line is state held between lines, and a file could end mid-pair
    // in a way no single line revealed.
    {
        size_t arrow = detail::find_stream_arrow(line);
        if (arrow != std::string::npos)
        {
            std::string lhs = detail::trim(line.substr(0, arrow));
            std::string rhs = detail::trim(line.substr(arrow + 2));
            if (lhs.empty() || rhs.empty())
                return CmdError{"stream: expected <a>.Produce(...) -> <b>.Consume(...)"};

            detail::DottedCall prod, cons;
            std::string err;
            if (!detail::parse_dotted(lhs, prod, err))
                return CmdError{"stream producer: " + err};
            if (!detail::parse_dotted(rhs, cons, err))
                return CmdError{"stream consumer: " + err};

            // Both ends are module actions. A reserved operation is not a
            // stream end -- spawn/attach/ensure/kill/unflag produce and
            // consume nothing.
            if (!detail::is_uppercase_start(prod.member)
             || !detail::is_uppercase_start(cons.member))
                return CmdError{"stream: both ends must be module actions "
                                "(TitleCase); reserved operations cannot stream"};

            CmdAction cmd;
            cmd.receiver          = prod.receiver;
            cmd.action            = prod.member;
            cmd.payload           = prod.payload;
            cmd.is_stream         = true;
            cmd.consumer_receiver = cons.receiver;
            cmd.consumer_action   = cons.member;
            cmd.consumer_payload  = cons.payload;
            return cmd;
        }
    }

    // ---- Dotted line: <name>.something( ... ) -----------------------------
    //
    // Tested before the bare-verb split so a receiver never has to avoid
    // colliding with a keyword: `spawn.Start()` is an action on an entity
    // named `spawn`, unambiguously, because the dot comes before any space.
    {
        size_t dot = line.find('.');
        size_t sp  = line.find_first_of(" \t");
        if (dot != std::string::npos && (sp == std::string::npos || dot < sp))
        {
            detail::DottedCall call;
            std::string err;
            if (!detail::parse_dotted(line, call, err))
                return CmdError{err};

            // After the dot: lowercase is a runtime operation, TitleCase is a
            // module action. The same split the top level uses, in the same
            // place -- and structural rather than probabilistic, because the
            // marketplace boundary already enforces TitleCase action names, so
            // no module action can ever be spelled `spawn` or `kill`.
            if (detail::is_lowercase_start(call.member))
            {
                if (call.member == "spawn" || call.member == "attach"
                 || call.member == "ensure")
                {
                    CmdChildAcquire cmd;
                    cmd.verb = (call.member == "spawn")  ? AcquireVerb::Spawn
                             : (call.member == "attach") ? AcquireVerb::Attach
                                                         : AcquireVerb::Ensure;
                    cmd.parent_name = call.receiver;
                    if (!detail::parse_typed_declaration(call.payload, cmd.module,
                                                         cmd.tag, cmd.name, err))
                        return CmdError{call.member + ": " + err};
                    return cmd;
                }
                if (call.member == "kill")
                {
                    if (call.payload.empty())
                        return CmdError{"kill: expected a work-function label, "
                                        "e.g. 'web.kill(Listen)' or 'web.kill(Listen 1)'"};
                    CmdKill cmd;
                    cmd.receiver = call.receiver;
                    size_t s2 = call.payload.find_first_of(" \t");
                    cmd.label = (s2 == std::string::npos) ? call.payload
                                                          : call.payload.substr(0, s2);
                    std::string idx = (s2 == std::string::npos) ? ""
                                    : detail::trim(call.payload.substr(s2 + 1));
                    if (!idx.empty())
                    {
                        try {
                            size_t end;
                            unsigned long long v = std::stoull(idx, &end);
                            if (end != idx.size()) throw std::invalid_argument("trailing");
                            cmd.index     = static_cast<size_t>(v);
                            cmd.has_index = true;
                        } catch (...) {
                            return CmdError{"kill: invalid index '" + idx + "'"};
                        }
                    }
                    return cmd;
                }
                if (call.member == "unflag")
                {
                    if (call.payload.empty())
                        return CmdError{"unflag: expected a flag name"};
                    return CmdUnflag{call.receiver, call.payload};
                }
                return CmdError{"'" + call.member + "' is not a runtime operation. "
                                "Lowercase after the dot is reserved (spawn, attach, "
                                "ensure, kill, unflag); a module action is TitleCase."};
            }

            CmdAction cmd;
            cmd.receiver = call.receiver;
            cmd.action   = call.member;
            cmd.payload  = call.payload;
            return cmd;
        }
    }

    // ---- Bare line: a verb, and what it acquires or launches --------------
    size_t sp = line.find_first_of(" \t");
    std::string verb = (sp == std::string::npos) ? line : line.substr(0, sp);
    std::string rest = (sp == std::string::npos) ? "" : detail::trim(line.substr(sp + 1));

    if (verb == "exit" || verb == "quit") return CmdExit{};

    if (verb == "requires")
    {
        if (rest.empty())
            return CmdError{"requires: expected a name"};

        std::string name = rest;
        std::vector<std::string> tags;

        size_t open = rest.find('[');
        if (open != std::string::npos)
        {
            size_t close = rest.find(']', open);
            if (close == std::string::npos)
                return CmdError{"requires: unbalanced '[' in tag list"};
            for (size_t i = close + 1; i < rest.size(); ++i)
                if (!std::isspace((unsigned char)rest[i]))
                    return CmdError{"requires: unexpected content after ']'"};

            name = detail::trim(rest.substr(0, open));

            std::string body = rest.substr(open + 1, close - open - 1);
            std::stringstream ss(body);
            std::string item;
            while (std::getline(ss, item, ','))
            {
                std::string t = detail::trim(item);
                if (t.empty()) continue;
                if (!detail::is_valid_requires_tag(t))
                    return CmdError{"requires: '" + t + "' is not a tag. A tag is "
                                    "either a bare is-a marker (Gate) or an "
                                    "origin-affixed one (Module::Tag); lowercase "
                                    "flags are not testable here."};
                tags.push_back(t);
            }
            if (tags.empty())
                return CmdError{"requires: empty tag list -- omit the brackets to "
                                "accept any live entity"};
        }

        if (name.find("::") != std::string::npos)
            return CmdError{"requires: '" + name + "' is a type, and requires takes a "
                            "name -- what the caller must hand in. What the entity has "
                            "to be able to DO goes in the tag list instead: "
                            "'requires <name> [" + name + "]' if that is a tag it "
                            "carries."};
        if (!detail::is_valid_local_name(name))
            return CmdError{"requires: '" + name + "' is not a valid name"};

        return CmdRequires{name, std::move(tags)};
    }

    if (verb == "spawn" || verb == "attach" || verb == "ensure")
    {
        CmdAcquire cmd;
        cmd.verb = (verb == "spawn")  ? AcquireVerb::Spawn
                 : (verb == "attach") ? AcquireVerb::Attach
                                      : AcquireVerb::Ensure;
        std::string err;
        if (!detail::parse_typed_declaration(rest, cmd.module, cmd.tag, cmd.name, err))
            return CmdError{verb + ": " + err};
        return cmd;
    }

    if (verb == "detach" || verb == "run")
    {
        if (rest.empty())
            return CmdError{verb + ": expected a script filename"};
        std::istringstream iss(rest);
        std::string script_tok;
        iss >> script_tok;

        std::vector<std::pair<std::string,std::string>> bindings;
        std::string err;
        if (!detail::parse_bindings(iss, verb.c_str(), bindings, err))
            return CmdError{err};

        if (verb == "detach") return CmdDetach{script_tok, std::move(bindings)};
        return CmdRun{script_tok, std::move(bindings)};
    }

    if (auto why = detail::removed_construct(verb))
        return CmdError{"'" + verb + "' is no longer a line: " + *why};

    // Bracketed, but with nothing before the dot to act on -- `Start()`, or
    // the old fully-qualified `Module::Tag.Action(...)`. Both are actions
    // missing the one thing every action now states, so say that rather than
    // falling through to the bare-declaration message below, which would send
    // the author looking for a spawn they do not need.
    if (line.find('(') != std::string::npos)
        return CmdError{"'" + line + "': an action names the entity it acts on. "
                        "Write '<name>." + verb.substr(0, verb.find('(')) + "(...)', "
                        "where <name> was introduced by requires, spawn, attach or "
                        "ensure."};

    // A bare TitleCase token used to declare a module or a tag, or dispatch an
    // action against whatever was in context. All three are gone, and the
    // error says which of the four verbs the author probably wanted rather
    // than reporting an unknown command -- this is the single most common
    // shape in every script written against the previous grammar.
    if (detail::is_uppercase_start(verb))
        return CmdError{"'" + line + "': a bare type name declares nothing. Write "
                        "'spawn Module::Tag <name>' (always new), 'attach "
                        "Module::Tag <name>' (must already exist), 'ensure "
                        "Module::Tag <name>' (either), or 'requires <name>' "
                        "(supplied by the caller). An action is "
                        "'<name>.Action(...)'."};

    return CmdError{"unknown command: '" + verb + "'"};
}

} // namespace ETCS

#endif // COMMAND_H__