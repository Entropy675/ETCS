# The .etcs Scripting Language

A `.etcs` file is a **sequential causal trace** — a record of named actions on named entities,
executed in order, fully deterministic and fully replayable. It is not a general-purpose
programming language. There are no branches, no loops, no mutable variables. What it has instead
is more precise: a structured way to declare which entities exist, what roles they occupy, and
what actions they perform on each other.

Two properties hold everything else up.

**No line is conditional.** So the set of lines that *could* run and the set that *will* run are
the same set, and a script's entire structure — including the structure of every script it
launches — is knowable before the first line executes. The runtime uses that: a whole tree is
resolved and checked at load, and a tree that cannot work is refused rather than discovered
halfway through.

**No line's meaning depends on what a previous line left behind.** Every line names the entity it
acts on. There is no cursor, no ambient selection, no state carried between lines that changes
what the next one means. A line's meaning depends only on which names are in scope, which is a
static fact about the file, not a runtime one about the moment.

---

## Core Concepts

### The address

An entity type has a three-level address:

```
Module :: Tag . Action
```

- **Module** — the loaded provider library (e.g. `DatabaseProvider`)
- **Tag** — the entity type within that module (e.g. `LocalDatabase`)
- **Action** — the operation to dispatch (e.g. `Connect`, `QueryProduce`)

A script writes the full `Module::Tag` exactly once per entity, on the line that acquires it and
binds it to a **name**. Every line after that names the entity rather than its type:

```etcs
spawn DatabaseProvider::LocalDatabase primary
primary.Connect(./data.db)
primary.ExecuteRaw(PRAGMA journal_mode=WAL;)
```

### Names

Names are the closest thing `.etcs` has to variables, and they are deliberately less than
variables: they are **named slots** binding a human-readable role to one entity instance
(identified internally by a RID). A name is never reassigned, only introduced — and it never
outlives the script that introduced it.

A name is introduced exactly once, by exactly one of four verbs. Which verb you use is the
statement of where that entity is expected to come from, and the runtime holds you to it.

### Tags, and the two kinds

Every entity carries a set of upper-case, origin-marked identifiers that answer "what is this,"
apart from — and in addition to — its own concrete `Module::Tag`. There are two kinds, and they
behave differently, which matters as soon as anything checks them:

- **Bare tags** — `Deletable`, `Gate`, `Parser` — are set once, when the entity is constructed,
  by whichever CRTP ancestor families its concrete type derives through. Fixed for the entity's
  whole life. Nothing a script can do adds or removes one; `unflag` reaches a completely separate,
  ordinary, freely-mutable set (see **Reserved operations**), never this one.
- **Origin-affixed tags** — written the same way a child's own address would be, e.g.
  `NetworkProvider::TLSContext` — record that this specific entity has, at some point in *this
  specific trace*, had a child of that type spawned or attached onto it. Not fixed at
  construction. Not a property of the concrete type at all — two entities of the identical
  concrete type can carry different origin-affixed tags depending on what their own causal
  history actually did to them. There is no `Module::Tag` you could hand to `spawn` that comes
  into existence already carrying one, because carrying one is the record of an action having
  already happened, not a property of a class.

Both kinds matter to `requires`, and matter differently — see below.

---

## The four acquisition verbs

Every entity a script touches enters through one of these. There is no implicit form — a bare
type name is not a declaration, and acting on a name that was never acquired is an error, not an
invitation to conjure something.

```etcs
requires <name> [Tag1, Tag2, ...]   # must already exist and qualify -- no Module::Tag, ever
spawn    Module::Tag <name>         # always creates a new entity
attach   Module::Tag <name>         # the closure's existing entity -- refused if there isn't one
ensure   Module::Tag <name>         # the closure's entity, or a new one if there isn't
```

All four require a name, because the name is the only thing the rest of the script can use.

`spawn`, `attach`, and `ensure` also have a **receiver-scoped form**, `<parent>.spawn(...)`,
`<parent>.attach(...)`, and `<parent>.ensure(...)`, which do the same three things with a parent
entity as the scope instead of the script's own closure. See **Children**. `requires` has no
receiver-scoped form — see why in that same section.

### `requires` — a parameter, filtered by slot rather than type

```etcs
requires game
requires server [Gate]
requires server [Deletable, Gate, NetworkProvider::TLSContext]
```

Declares that this script needs something already in scope answering to the name `game` (or
`server`), and, if a bracket is given, that it also answers to every tag listed. No spawn, no
create, no fallback of any kind. If nothing qualifies, the script does not run at all.

`requires` never takes a `Module::Tag`, and that is still deliberate: a script that declares
`requires game` states that it needs *something* named `game`, and everything it then does with
that entity — an action, a route registration, a `@game` substitution — is the real statement of
what it needs the entity to be able to do. Naming one exact concrete type would say more than the
script means; two callers passing differently-typed things that both answer the same actions are
the same script with a different binding.

What `requires` *can* name, without naming a type, is the entity's **slot** — what it is
preconfigured to be able to do, right now, as a specific already-built thing. That is exactly why
this is a `requires`-only capability rather than something `spawn`/`attach`/`ensure` could also
carry: those three already name an exact `Module::Tag`, which is strictly more specific than any
tag list could be. Naming the type already tells you every bare tag that type will ever carry, so
a tag list next to a `Module::Tag` can never actually discriminate anything — it is either always
satisfied (redundant) or never satisfiable (caught the moment the type is named, whether or not
`requires` is even involved). Tags have content only in the one place a type isn't already pinned.

A tag list can mix both kinds from **Tags, and the two kinds** above, freely:

```etcs
requires server [Deletable, Gate, NetworkProvider::TLSContext]
```

says `server` must be a bare `Deletable` and `Gate` (whatever its concrete type is), *and* must
already have had a `TLSContext` spawned or attached onto it by the time this script starts.

**Where `requires` looks.** `requires` resolves through the same closure lookup `attach` uses: an
explicit `name=value` binding passed on this script's own launch line, if there is one, else a
root global of the same name. An explicit binding shadows a same-named global exactly the way any
local name does. This is deliberate, not an accident of implementation: if `requires` only ever
looked at explicit bindings, a script wanting to assert a tag invariant on something that is
*also* naturally reachable as a global would have exactly two bad options — thread it as a binding
through every intermediate `detach`/`run` purely so `requires` could see it, or accept that the
invariant silently does not hold whenever the entity arrived by ordinary global visibility instead
of an explicit pass-through. Checking both sources closes that gap: the tag invariant a script
declares holds no matter which path the entity actually arrived by.

It never resolves against a name this same script would go on to introduce itself later — which
costs nothing to state as a rule, because it is already impossible: a name can be `requires`'d or
`spawn`/`attach`/`ensure`'d, never both, and `requires` lines are checked before this script's own
body has run a single line.

**When each half is checked.** The two kinds of tag do not become knowable at the same time, so
they are not checked at the same time:

- Whether a qualifying binding or global exists *at all*, and whether a bare tag could ever be
  satisfied by whatever concrete type that name traces back to, is knowable from the tree's
  structure alone — refused **at load**, in the same pass that catches type disagreement, before
  any line anywhere in the tree runs.
- Liveness of the resolved RID, and origin-affixed tag membership, are facts about *this specific
  entity's causal history so far* — not knowable without either simulating the whole tree's causal
  order ahead of time, or simply waiting until execution has actually reached this point. The
  runtime already has to stop and check liveness right when a `requires`-bearing script would
  start; origin-affixed tags are read at that exact same stop, at no extra cost in machinery.

Both halves report the same outcome — `ExecuteStatus::Unmet`, "this script fails to run because
what was given it does not meet its requirements" — just at the earliest point each half actually
allows.

`requires` lines are collected across the **whole file** before the first line runs, not just the
ones at the top. Placement is a readability choice; the check is not positional. All unsatisfied
requirements are reported together.

### `spawn` — a new entity

```etcs
spawn NetworkProvider::HttpServer web
```

Always creates. Never reuses, never retargets, never finds something that already exists. If you
want the entity to be new, this is the only verb that guarantees it.

### `attach` — the closure's entity, strictly

```etcs
attach NetworkProvider::HttpServer web
```

Binds the entity named `web` in this script's closure — an explicit binding, if this script was
given one, else a root global. If there is neither, the script does not run. `attach` never
creates. It is a pure lookup, with exactly one outcome: found and correctly typed, or refused.

Introducing a name twice in one script is a contradiction and is refused before the script runs.
There is no re-selection to do — a name, once introduced, is used by naming it.

The `Module::Tag` on an `attach` is a type assertion as well as an address: resolving `web`
through the `NetworkProvider::HttpServer` list finds nothing if `web` is bound to some other kind
of entity, and that is reported as a type disagreement rather than as a missing name. A name that
does not exist anywhere reachable is reported exactly the same way — refused at load, same pass,
same category. There used to be a real difference between these two failures; there no longer is.

### `ensure` — the closure's entity, or a new one

```etcs
ensure NetworkProvider::HttpServer web
```

Binds the entity named `web` in this script's closure if there is one, and creates it otherwise.
This is the only verb with two outcomes, and now that is stated by its name rather than left
implicit in `attach`: a script written with `ensure` runs standalone (creating what it needs) and
runs as part of a larger tree (using what the tree already has) with no other difference between
the two. Reading `ensure` on a line tells you, on sight, that this might create and might reuse,
by design — reading `attach` now tells you the opposite: this line guarantees reuse or nothing.

---

## Closures and globals

A name belongs to the script that introduced it and dies with that script. There is no
process-wide name table; two scripts can both use the name `web` for unrelated entities and
never see each other's.

A script's **closure** is:

1. the names it introduced itself, plus
2. the names it was passed as bindings, plus
3. the **globals**.

The **globals** are the names introduced by the *root* script — whichever script was invoked
directly, the one whose lifetime is the runtime's lifetime. Every script anywhere in the tree
below it can `attach` or `ensure` those names, and `requires` can be satisfied by them directly,
with no binding needed at any of the hops in between.

That is the whole scoping rule. Two scopes, local and global, and no chain in between: a script's
*parent's* names are not visible to it, only its own, the ones it was explicitly handed, and the
root's. This is the same shape as function locals versus module globals — your caller's locals
are not in scope, globals are — and it is what makes the resolution of every name in a tree
decidable rather than dependent on how deep something happens to sit.

```etcs
# run_tls_website.etcs -- the root. Its names are the runtime's globals.
spawn NetworkProvider::HttpServer web
...
detach chess_lobby.etcs
```

```etcs
# chess_lobby.etcs -- says nothing about web; does not need to.
spawn ChessProvider::ChessNode node
node.Mount(game)
detach chess_web.etcs game=node
```

```etcs
# chess_web.etcs -- ensure finds the root's web two levels up, because it is
# global rather than inherited. Run this file on its own and the same line
# creates a server instead.
requires game
ensure NetworkProvider::HttpServer web
web.SetPort(8080)
```

---

## Actions

An action is always written on the entity it acts on, and its arguments are always bracketed:

```etcs
<name>.Action(payload)
<name>.Action()            # no arguments
```

Both halves are mandatory, and both exist for the same reason — to remove a way a line could mean
something other than what it says.

**The receiver is mandatory** because the alternative is an ambient "current entity", and an
ambient current entity is state a previous line left behind. `Connect(./data.db)` cannot be read
without knowing what came before it and how far back; `primary.Connect(./data.db)` can be read on
its own, from a log, out of order, forever. What this arrives at is the ordinary
`receiver.method(args)` of every language that has ever had objects, and it arrives there for the
reason that form exists rather than by imitating it.

**The brackets are mandatory** because the previous form had a rule for where the payload started
— a leading token was silently dropped if it happened to name the entity in hand — and that rule
was the one place a line's meaning depended on runtime state. `ListPaths tree` was either
"ListPaths with no arguments" or "ListPaths with the argument `tree`" depending on what `tree` was
bound to at that moment, which means the same text could not be read without running it. It had
already produced a real misparse: a `Listen` config carried an unstripped selector into a work
function whose numeric parse silently skipped the non-numeric token, and a default port that
happened to match what was wanted hid it completely.

With both, `tree.ListPaths()` takes no arguments and `tree.ListPaths(x)` passes the string `x`,
in every context, forever.

### Payloads

Whatever is between the brackets is handed to the work function untouched, with one substitution.

**`@name` substitutes a RID.** A payload token written `@name` is replaced by the RID that name
is bound to. This is how a script hands one entity to another without ever writing a number:

```etcs
web.AddHandler(@tarpit Request @tarpit Filter)
```

The sigil is required rather than substituting anything that happens to match a name, because
payloads legitimately carry paths, titles and free text that could collide.

**Matching the closing bracket.** Payloads nest — SQL is full of parentheses — so the closer is
found by depth counting, and the counter is **quote-aware**: parentheses inside a `'…'` or `"…"`
span are literal text and are not counted. `t.ExecuteRaw(SELECT ')' FROM x)` therefore works. A
payload carrying unbalanced parentheses outside quotes has to be quoted; there is no escape
character.

Nothing but whitespace may follow the closing bracket, and an action is one line. A payload does
not continue across a newline even if its brackets are unbalanced at the end of one — a trace is
a sequence of lines, and a line that cannot be read alone is not one.

### Reserved operations

After the dot, **lowercase names are runtime operations and TitleCase names are module actions**.
The same split the top level uses, in the same place. The reserved set is small:

```etcs
web.spawn(Module::Tag <name>)     # new child
web.attach(Module::Tag <name>)    # this parent's child by that name -- refused if there isn't one
web.ensure(Module::Tag <name>)    # this parent's child by that name, or a new one
web.kill(<label>)                 # interrupt every live call carrying this label
web.kill(<label> <index>)         # interrupt one, by position among live calls of that label
web.unflag(<flag>)                # remove a flag -- the freely-mutable set, not a `requires` tag
```

`kill` requests; it does not wait. A call stops when its own body next checks, which for a call
mid-syscall may not be immediately. A script that needs the work to have genuinely stopped has to
observe that some other way.

---

## Streams

Some actions open a data stream: one entity produces, another consumes, and the two are written
as one line with `->` between them.

```etcs
forumdb.RowProduce(SELECT * FROM forum_thread) -> node.LoadRows()
```

The two ends may be different entities, different tags and different modules. Nothing else is
required to plumb them — the runtime resolves both handles and routes the pipe.

There is no two-line form. A produce on one line and a consume on the next would be exactly the
thing the rest of this design removes: state held between lines that changes what the following
line means, and a file that can end mid-pair in a way no single line reveals. One line, both
ends, or it is not a stream.

---

## Children

`spawn`, `attach`, and `ensure` on a receiver create or find a **typed child** of that entity:

```etcs
web.spawn(NetworkProvider::StaticHtmlPage landing)
landing.SetHtmlFromFile(./www/index.html)

web.attach(NetworkProvider::FileHtmlPage tree)
tree.LoadFromDisk(./www)
```

`spawn` on a receiver always creates. `attach` on a receiver binds the child **of this parent**
already carrying that name, and refuses if there isn't one — the parent is part of the test, not
just the search order, so a `tree` belonging to some other server never binds here, and never
silently manufactures a look-alike either. `ensure` on a receiver is the two-outcome form: binds
this parent's existing child of that name, or creates one.

That parent test is also why the top-level forms are not substitutes for the receiver-scoped
ones. Top-level `spawn`/`attach`/`ensure` act on the script's own closure or the globals; the
receiver-scoped forms act on one specific parent's children, and the parent's identity is always
part of the match. Writing `attach NetworkProvider::FileHtmlPage tree` at the top level where you
meant `web.attach(...)` finds a *global* `tree`, if one happens to exist, or refuses — it never
reaches into `web`'s own children by accident.

There is no receiver-scoped `requires`: a child is not something a caller can hand you, and an
already-built child of a specific parent is not a slot description — it either is that parent's
child or it is not, which `attach`/`ensure` on the receiver already say precisely.

---

## Sub-scripts

`detach` and `run` launch another `.etcs` file with an optional set of bindings:

```etcs
detach chess_lobby.etcs
detach chess_web.etcs game=node
run    forum_web.etcs forum=node
```

`detach` starts the sub-script and continues; `run` starts it and waits. Either way the sub-script
gets its own `Root`, its own signal context, and its own closure — the names on the right of `=`
are looked up in *this* script's own closure (its own names, what it was passed, or a global) and
handed to the sub-script under the name on the left.

A sub-script's `requires` must be satisfied by the bindings on its launch line, or by a root
global reachable without any binding at all — see **Where `requires` looks**. Since there is no
branching, whether a given `detach`/`run` target `requires` is satisfiable is knowable by reading
the whole tree, so an unsatisfiable target requires is refused at load, not at the moment it is
reached.

---

## What is checked before anything runs

Invoking a script resolves its entire tree — every `detach` and `run` target, recursively — reads
all of it, and checks it. Nothing executes until that passes. Because there is no branching, this
rejects nothing that would otherwise have worked; it only moves the failure earlier.

Refused at load:

- **Unsatisfiable `requires`** anywhere in the tree — no binding and no reachable global exists
  for a name a script demands, or one exists but its concrete type could never carry a bare tag
  the script lists. (Origin-affixed tags and liveness are the two things this pass cannot know —
  see **When each half is checked**; those are refused live, at the same `Unmet` outcome, the
  moment the script would actually start.)
- **A name introduced twice** in one script.
- **A receiver that was never introduced** — `web.Start()` in a script with no `web`.
- **An `attach` (top-level or receiver-scoped) that cannot resolve** — no matching name in scope
  at all, or one exists but is a different type. Both are refused the same way; there is no longer
  a difference between "missing" and "wrong kind" for this verb, because `attach` never falls back
  to creating either way.
- **Name collisions across the tree** where a local shadows a global of a *different* type. A
  same-type shadow is annotated rather than refused: independent scripts reaching for an obvious
  name for an obvious thing is what locals are for.
- **Cycles** — `a.etcs` launching `b.etcs` launching `a.etcs`. With no branching there is no base
  case, so a cycle is never anything but a bug.

The same pass produces, for every `attach` and `ensure` in the tree, whether it will bind an
existing entity or (for `ensure` only) create a new one. That resolution is the runtime's, and
tooling renders it rather than recomputing it — there is one implementation of what a name means,
and it is the one that runs.

---

## How a script stops

| | |
|---|---|
| **end of file** | normal completion |
| **`exit`** | deliberate early stop, not a failure |
| **unmet requirement** | a `requires` (or a strict `attach`) was not satisfied — the script never started |
| **vanished dependency** | see below |
| **fatal** | an action threw something the runtime could not attribute |

### A failed action is not a stop

An action that fails on its own terms — a payload the work function rejects, a `SetPort` refused
because the server is already listening — reports the failure and the script continues. That is
deliberate. A trace records what was attempted and what came of it, and a refusal is a result,
not a broken transcript. `run_tls_website.etcs` depends on exactly this: `chess_web.etcs` carries
its own `web.SetPort(8080)`, and against an already-started server on 8443 that is *supposed* to
be refused with a visible line rather than taken or fatal.

### The liveness rule

Every RID a script `spawn`s, `attach`es, `ensure`s, or is passed — its closure — is watched. If a
script names a receiver in its closure and that entity no longer resolves, the script stops **at
that line**, not before and not on some later line that never mentions it. There is exactly one
check site: the receiver of a dotted line. No cursor, no pending-stream producer, no separate
sweep between lines — just: resolve the receiver, and if it is in the closure and does not
resolve, stop.

A `detach`ed script that vanishes just ends — its thread returns, and it stays in the job table
until shutdown. A `run` script that vanishes returns control to its caller at the next line; the
caller is not killed, though it may separately vanish on its own next reference. The root script
vanishing ends the runtime.

---

## Comments and directives

`#` starts a comment, to end of line. `#IMPORT` and `#EXPORT`, conventionally on line two, name
the domain folders a `detach`/`run` target is resolved against; unchanged by anything in this
document.

## ABI integrity

Every module load checks an environment-signature hash against what each exported symbol was
built with. A mismatch refuses the load rather than risk a silently incompatible call. Unchanged.

## Shutdown

Loader unload and the root's own vacate are ordered so a module is never unloaded while a call
into it could still be pending — see the loader-exit-path material for the mechanism. Unchanged
by anything here.

---

## Summary

| Concept | Mechanism |
|---|---|
| Required input | `requires <name> [Tags]` — no `Module::Tag`, optional tag-set constraint, resolves through the same closure lookup as `attach`, checked before the script runs |
| New entity | `spawn Module::Tag <name>` — always creates |
| Closure entity, strictly | `attach Module::Tag <name>` — binds an existing one, refuses otherwise |
| Closure entity or new | `ensure Module::Tag <name>` — binds the closure's, else creates |
| New typed child | `<parent>.spawn( Module::Tag <name> )` |
| This parent's child, strictly | `<parent>.attach( Module::Tag <name> )` — parent is part of the test |
| This parent's child, or new | `<parent>.ensure( Module::Tag <name> )` |
| Action | `<name>.Action(payload)` — receiver and brackets both mandatory |
| RID in a payload | `@name` |
| Stream | `<a>.Produce(payload) -> <b>.Consume(payload)` — one line, both ends |
| Interrupting work | `<name>.kill(<label> [index])` |
| Removing a flag | `<name>.unflag(<flag>)` — the mutable set; a `requires` tag is fixed and out of reach |
| Scope | local closure + the root script's names as globals; no ancestor chain |
| Passing a local down | `detach child.etcs k=name` / `run child.etcs k=name` |
| Comments | `#` prefix |
| Sub-script search path | `#IMPORT` / `#EXPORT` on line two |
| ABI check | automatic at module load |
| Whole-tree check | automatic at invocation, before any line runs |

Every line is one of two shapes. A **bare** line introduces a name or launches a script:
`requires`, `spawn`, `attach`, `ensure`, `detach`, `run`, `exit`. A **dotted** line acts on a
name: `<name>.something(...)`. Nothing else is a line.

---

## What changed from the previous version of this document

Removed outright:

- **The implicit declaration form.** A bare `DatabaseProvider` / `LocalDatabase primary` no longer
  declares anything. Every acquisition names a verb.
- **Implicit spawn-on-demand.** "No entity selected — spawning one automatically" is gone. A name
  that was never acquired is an error.
- **The `context` keyword**, and with it the whole notion of an ambient current entity. Every line
  names its own receiver, so there is nothing to switch.
- **Re-selection.** There is no such operation, because there is nothing to select. `attach`
  introduces a name; it is not a cursor move.
- **`as <name>`.** Names come from acquisition verbs only; a fourth, quieter binding path was the
  opposite of what the rest of this is for.
- **RID literals.** `context Module::Tag 12345` and bare-index selection are gone. Scripts use RIDs
  under the hood and hand them around as `@name`; you never type one.
- **Fully qualified actions** (`Module::Tag.Action payload`). Superseded by `<name>.Action(...)`,
  which addresses the instance rather than the type.
- **`.add()`.** Replaced by the receiver-scoped `spawn`/`attach`/`ensure`, the same verbs the top
  level uses rather than a fourth word for something one of them already meant.
- **Unbracketed payloads, and the leading-selector rule that came with them.** That rule was the
  one construct whose meaning could not be determined without running the script.
- **The two-line stream form**, and the pending-stream state it required. A produce whose consumer
  is on the next line is state held between lines, and a file could end mid-pair in a way no single
  line revealed.
- **Process-wide name persistence.** Names no longer survive the script that made them, and a
  re-run no longer silently retargets onto entities from a previous one. A root script's names are
  the runtime's globals for as long as that root is running, and that is the whole of it.

Refined, not removed:

- **`attach` split into `attach` and `ensure`.** `attach` used to be the only verb with two
  outcomes — bind the closure's entity, or create one if there wasn't one — and that ambiguity was
  never visible on the line itself, only in whatever the tree around it happened to contain.
  `attach` now means exactly one thing: bind an existing entity, or refuse. `ensure` carries the
  old two-outcome behavior under its own name, so a script that deliberately wants "reuse if
  present, create if not" still says so — it just says so explicitly instead of by omission.
- **`requires <name>` can now take a bracketed tag list.** The verb still carries no
  `Module::Tag` — that restriction was never the point, precision about the *type* was. The
  point was always the *slot*: what the entity has to be able to do. A tag list says that
  directly, checked against each type's fixed is-a set (and, live, against what it has since had
  attached to it) at the same load-time pass that already catches type disagreement, rather than
  left for the script's own actions to imply it silently.
- **`requires` now resolves through the same closure lookup `attach` uses**, including root
  globals, not only explicit bindings. Otherwise a tag invariant a script declares would silently
  not hold whenever the entity in question happened to arrive by ordinary global visibility
  instead of a threaded-through binding — the same name, the same entity, a real gap in the
  guarantee for no reason but which path it came in on.

Consequences worth knowing:

- Re-running a script creates fresh entities rather than reusing the previous run's. That is what
  `spawn` now means.
- Two scripts sharing an entity share it because someone passed it, because it is a global, or
  because `ensure` deliberately created and left it findable — never because they happened to pick
  the same name and got lucky.
- A script written with `attach` can no longer be run standalone; there is nothing above it to
  have created what it's looking for. A script that needs to work both standalone and inside a
  tree writes `ensure` instead — that trade is now a choice on the line, not an accident of what
  the tree around it happens to contain.
- The browse surface no longer executes `.etcs` lines typed at a prompt. It navigates the live
  entity graph and runs scripts; the language is for files.
