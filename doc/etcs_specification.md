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
requires <name> [Tag, ...]          # must already exist and qualify -- name here, type in the bracket
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

Where the other three verbs take a `Module::Tag`, `requires` takes a **name**. A type can still
appear — it just appears in the bracket, as something the binding must satisfy, rather than in the
address slot, as something to create or find:

```etcs
requires window [Window, WindowProvider::GLFWWindow]
```

That is why the bracket is a `requires`-only capability. The other three pin their type in the
address slot already, so a tag list beside it could never discriminate anything: naming the type
tells you every tag that type carries, making the list either redundant or unsatisfiable. Tags
have content only where the address slot holds a name instead.

The bracket is also a **choice of how much to assert**. `requires db [Database]` and
`requires db [LocalDatabase]` are different claims, and the narrower one is the stronger. A script
that only uses the `Database` part of an interface says so with the wider tag, and then serves any
concrete type in that family.

What a family tag promises is membership, not a callable surface. Every method a family declares
exists on every type claiming it — the CRTP proxy makes it a pure virtual, so a leaf that skipped
one would not compile. What varies is how much of it a type **exports as work functions**, and
that is the only thing a script's reach depends on. A method that is not exported is not missing:
it is C++-only, reached by whatever code holds the concrete type, and reached from a trace by
handing the RID to a work function that uses it — which is how `run_tls_website.etcs` drives
parsers and TLS contexts without ever naming one of their constraint methods.

So a tagged `requires` gates on what the entity *is*; the actions the script writes are what
determine what it needs to be able to *call*. Where those disagree the failure is deterministic —
same binding, same line — and, since a failed action is not a stop, recorded rather than fatal.
`ace abi` prints both surfaces per type, which is where the correspondence is worth reading.

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

**Refused if the name is already in scope** — local or global. Overwriting a global is a real
mechanism (one runtime, one table, later writes win); doing it by accident is not. `spawn` means
"make a new one", so a word that already answers to something was almost certainly meant to reach
it, and the refusal says `attach`/`ensure`. Applies to `<parent>.spawn(...)` the same way.

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
web.spawn(NetworkProvider::LinkHub links)
...
run ChessProvider/scripts/chess_service.etcs links=links
```

```etcs
# chess_service.etcs -- says nothing about web; it was handed the hub.
requires links [LinkHub]
spawn NetworkProvider::Lobby hall_list
...
run ChessProvider/scripts/chess_ranked_table.etcs links=links hall=hall_list
run ChessProvider/scripts/chess_ranked_table.etcs links=links hall=hall_list
```

```etcs
# chess_ranked_table.etcs -- run twice above, and its names are its own each
# time: `game` here is not the other run's `game`, and neither is a global.
requires links [LinkHub]
requires hall [Lobby]
spawn ChessProvider::ChessGame game
...
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
payload carrying unbalanced parentheses outside quotes has to be quoted.

Inside a quoted span a backslash escapes the next character, so `\'` is a literal apostrophe and
does not close the quote — matching what `TBuffer::operator>>(std::string&)` does when a work
function extracts a quoted argument. The parser skips the escape without interpreting it; what
reaches the work function is byte-for-byte what was written between the brackets. That agreement
is what makes `db.Q('it\'s')` one argument rather than a truncated payload.

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

**Where a produce body runs.** The consumer runs on the caller's thread and owns the pair's frame;
the producer's body is a task on its module's pool — unless it STANDS. A body that follows a
record, watches a listing or emits a page's changes for as long as a session lasts is declared
`DEFINE_STREAM_FUNC_PRODUCE_STANDING` and gets a thread of its own, since a pool worker held for
the life of an edge is a worker the module no longer has (a web build's pool is two). And a half
run for another runtime's pair (a link's edge, `Entity::produceOnto`) runs on the thread that
runs it, whichever kind of body it is. A standing body learns its reader has gone without writing
(`MirrorBuffer::readerGone`), so a follower that leaves is noticed while the record is quiet.

### Across runtimes

Either end of a stream, and the receiver of any action, may be an entity that lives in **another
runtime** — a browser tab, a process on another machine. Nothing about the line changes; what
changes is how the name was bound:

```etcs
spawn NetworkProvider::Peer peer
peer.Connect(wss://example.org/link/studio)

spawn NetworkProvider::Ledger book           # the SAME type as the host's node
book.spawn(NetworkProvider::Remote remote)     # makes book a surface of a far node
remote.Bind(book @peer)                        # ...the one the host published as `book`

spawn NetworkProvider::Ledger copy
book.Append(guest a line)                      # runs on the host
book.Follow(0) -> copy.Mirror()                # host produces, this side consumes
```

`book` is a **surface**: a local instance of the far node's type, so it is everything that type is
to every family walk and `requires` bracket. The `Remote` child is the mark that it is not local,
and the wire: from `Bind` on, its actions run on the far node and come back with that node's
answer, and a stream naming it runs its half there while this runtime runs the other. The
caller still names both halves, exactly as in-process. The link's own verbs (`Bind`, `Unbind`,
`Info`) are the child's, so nothing the far node answers to is shadowed by something only this
side has.

**One link per pair of runtimes.** However many surfaces are bound through a `Peer`, and however
many actions and streams run on them, they share its one connection: a stream is a numbered
channel on it, not a connection of its own. A second `Peer` to an address one already holds is
refused.

**An answer is waited for as long as the link lives.** An action on a surface, a `Bind`, a stream
opening: each holds its caller as a local action does, until the far side answers, the link
closes, or the caller is interrupted. So does making the link: a guest waits for its welcome, and
a hub keeps it parked, until a host hosting through the hub comes for it. Whether the far side is still there is the transport's to
say -- a server's end pings every 10 s, and either end closes a link it has heard nothing on for
35 s, which wakes every caller waiting on it. How long the far side takes is not: one that is busy
(a browser tab moves its socket's bytes only when its main thread runs) answers late, not never.

**A browser is a runtime like any other.** The web build dials the same way (its socket is the
browser's WebSocket), so a page's runtime binds surfaces and opens streams on a native one
directly. A page cannot accept a connection, so a page that hosts does it through a hub
(`room.HostVia`, below), which splices bytes and understands none of them.

**A stream's unit is a message, of any size.** An action's payload is a `Buffer`, but a stream
half may write and read whole messages (`MirrorBuffer::writeMessage` / `readMessage`): a page, a
photograph, a record's line. Each goes as a run of frames and arrives as one, on every strategy —
in process, between processes, and across a link — and a message that fits one frame is an
ordinary frame.

**Same code, same authority.** A surface binds only to the same build of its type (the tag hash,
below, **ABI integrity**), and each action and stream half names its own hash, which the far side
checks. And it binds only if it carries the far node's **authority layer**: the node's `Wrapper`
children on the network scope, which the script attaches to the surface like any child. Every
frame to or from the node passes them, and a stage can refuse one — a `NetworkProvider::Seal` does,
unless both sides hold the same key — which refuses the action or ends the stream. A wrapper this
runtime cannot construct cannot be attached, so that node cannot be reached at all; only what else
the far side publishes, which reaches it there, can.

**The local frame never crosses.** A `Database` is a resource of a locale — it is somewhere
before it is anywhere — so its surface is this runtime's only, and neither side binds or opens a
stream onto one; nor onto anything else that carries the bare `Local` tag, which is how
`DatabaseProvider::Persistence` says its verbs are this loader's store. What a database holds
reaches another runtime only through a type that is published and reads it here.

**A surface starts as a reflection.** When the far node claims `Environmental` (below, **What
outlives the runtime**), `Bind` brings its named state along and the surface puts it back through
`ReflectRemote` — a guest's `Ledger` surface holds the host's lines from the moment it binds.

The far side decides what is reachable: a `Room` publishes entities by name
(`room.Publish(book @book)`) — and by verb, when the publication lists them
(`room.Publish(minutes @minutes Head Since Follow)`: a record published to be read) — and nothing
else in that runtime can be bound. The way in to a record published that way is a *door*: a
sealed `Ledger` fed into it (`door.Feed(@minutes)`), so writing takes the key and reading does
not. One guest per name: a second link under a name already linked is refused at the hello. And
the link is symmetric — what a guest publishes on its `Peer`, the host binds through the `Room` by
the guest's name (`remote.Bind(mail @room alice)`), the one channel that is that guest's alone. The host of a room is its
authority — every guest's actions arrive there and run in its order — whether it serves the room
itself (`room.Host(@links studio)`, beside an `HttpServer`'s `LinkHub`) or, unable to accept
connections, hosts through a server that only splices bytes (`room.HostVia(wss://site/link/studio)`).
Guests dial the same address either way.

Two families ride on this for anything shared: a **Directory** (`NetworkProvider::Lobby`) lists who
can be reached, each entry lasting as long as its advertiser's link, and watched as a stream
(`lobby.Watch() -> copy.Mirror()`: the listing whole, on every change); a **Record**
(`NetworkProvider::Ledger`) is lines in the host's order, chained by hash, authored by the link
each came over, and followed as a stream (`book.Follow(0) -> copy.Mirror()`). A line too big for an
action is streamed in (`x.Produce -> book.Take()`), and the host keeps the record from its last
**checkpoint** on (`book.Checkpoint(<seq>)`): a line that states everything makes those before it
history nobody replays, and a reader from before it restarts there.

---

## The frame edge

A session that shows anything has one tick, and two lines make it:

```etcs
view.Compose(@stage)                                         # what to draw: a root to walk
detach RenderProvider/scripts/render_frames.etcs view=view   # the tick: view.RunFrames(16)
```

`RunFrames` visits every entity that claims **Animated** (`ontology/Animated.h`) once a tick,
whichever module made it and in no order a leaf may rely on. A session needs exactly one caller
of it. Presenting is one such visit, the surface's own: it holds the composed root for the length
of the walk and draws what it finds.

Where that hold sits is the whole of what a type needs to know about a frame:

- **A walk reads.** Everything drawn in a frame is drawn inside the hold, and nothing inside a
  lifetime hold may emit an ordered event — a tag or a value changing, a child made, moved or
  deleted — because the delete it would queue behind may be waiting on that hold. Every such
  event asserts it.
- **A visit acts.** An Animated leaf's own visit is outside every hold. What a frame changes
  about the graph it changes there, and the picture that follows shows it.

So a thing that changes by being looked at — a scene whose bodies step, fit and move between
containers — is stepped on the edge and never in a picture. A picture notes that it looked; the
tick, once its visits are done, steps what they looked at; the next picture shows it
(`ontology/etcs_causal_constraints.md` §14).

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

## What a RID is, and where it stops

A name is bound to a RID, `@name` hands one over, and no script ever shows one. What it is
underneath is small, and it decides what a type may do with it.

**What it is.** The hash of three things: the module, the type, and how many of that type this
runtime had made before it (`generateRID`, `core/Entity.h`). No clock and no address. So the same
lines in the same order give the same RIDs — and a RID says where and when something was made,
nothing about what it is.

**Inside a run it is the reference.** A binding holds one. A type handed one in a verb's payload
keeps it and resolves it again at each use, through the family it needs (`resolve_held`,
`resolve_in_family`): the resolution is the liveness check, the same one dispatch makes for every
action. A RID orders nothing — events are ordered by which types they touch, not which entity.

**It does not leave the run**, and three boundaries see to that, so a type never has to:

- *the record* keeps a line as it was written, `@names` and all, beside the RIDs they meant
  then, and the script made from it names each entity by where it sits in what is rebuilt
  (**What is kept is how, not what**, below);
- *the hashes* hold none: identity is what a thing is, state is that and its values;
- *the store* finds a record by what the entity is, and hands its values back after the script
  has run.

**So a type that refers to another entity** takes the RID in a verb, keeps it, and puts a value
through the funnel beside it. The value is what makes the funnel record the line — only a change
is recorded — and the state hash reads it, so it names no RID and differs when the referent
does. It is not how the referent is found again: the replayed line hands the verb a RID of the
rebuilt graph, in the order the lines first ran. The value comes back afterwards
(`Entity::restoreValue`, with no verb behind it) saying what that line just set, and the type
keeps the RID it has.

What a type must not do is build its own way back from a value to an entity. That is the record's
rule in a second home, and the two disagree as soon as the graph changes after the line ran:
`Scene3D.SetMesh` first resolved an address at every picture, and a ball that fit took into the
cup lost its shape, because "one up" had become the cup.

---

## What outlives the runtime

Everything a script builds is gone when the runtime stops — except what asks to be kept:

```etcs
spawn NetworkProvider::Ledger book
book.spawn(DatabaseProvider::Persistence keep)   # book, and everything under it, is kept
book.Append(first line)
book.spawn(NetworkProvider::Ledger margin)
```

The next time the loader starts with no script it offers the scene back — *Continue where you
left off?* — and `etcs --resume [script.etcs]` takes it without asking (before the script, which
can then use it); `--fresh` declines. What comes back is `book` with its lines, `margin` with its
own, both answering to the names the scene gives them.

**Opt-in, by family.** A type that can be kept claims **Environmental** (`ontology/
Environmental.h`), and nothing else pays for it: from the moment an entity of such a type exists,
the runtime records, for every tag that goes on or off it (or on a plain entity beneath it), the
one script-level action it happened inside — the line, not the calls that line made. Small fast
types that live purely in the functional realm never claim it and never record anything.

**What is kept is how, not what.** The record is compacted to what the surface still shows —
every action that put on something still there, and every later action that took off something
those put on — and written out as ordinary ETCS lines: the script that makes it again. A child a
line made is named from its parent's name, its type and the order it was made in
(`book_Ledger1`); `@names` in payloads become those names; no RID is ever written.

**What is so is a value on the same surface.** A tag can carry a value (`core/Entity.h`, "THE
VALUE BEHIND A TAG"), and that is where state lives rather than in members beside the tags.
State a verb sets — a damping, a colour, an emissivity — is a *stored* value set through the
funnel with its flag, so the line that set it is kept and a replay sets it again. State that moves
without a verb — the Causal rows, a ledger's lines that came from whoever appended them — is a
*bound* value, read off the type on demand. The store keeps every value under the root and hands
them back after the script has run; an Environmental type then finishes (`RebuildLocal`). Replay is
deterministic by the rules of scripts, so anything that was not (a clock, input, a far node's
answer) belongs in a value, as its result. A type that renames a value later says how, oldest
first (`MigrateTo`); the ETCS surface only grows, so an old script still replays.

**The same script in both frames.** Rebuilding an entity here, from this runtime's store, and
reflecting it on the far side of a link are one operation seen from two sides: `RebuildLocal`
is the local frame's slot and `ReflectRemote` the far frame's, and a type says how it answers in
each.

**Looked up by what it is.** Each kept entity's record is keyed by its `Module::Tag` and its
identity hash — the merkle hash of its tags down through its children, over the compile-time
hashes of the code behind them and **no RIDs** (a RID is where and when something was made; a
scene made again in another order is the same scene) — and, among entities equal in both, by the
order they were made in, which a replay reproduces without storing it. A box that moved is the same
box, so its values do not enter the key. A replay that did not make what was saved finds no
record, and says so, rather than handing an entity someone else's state; and the resume ends by
checking every root's state hash — the identity and every value under it, one number — against
the one saved, naming which half differs if one does.

**This loader's own.** The store is `persistence.db` in `$ETCS_STORE`, else
`$XDG_DATA_HOME/etcs`, else `~/.local/share/etcs` (`/persist` in a browser, which the page mounts
on IndexedDB), beside a key made on first use that never leaves it. Every record and scene is
MACed with that key; one that does not verify is not replayed. It is saved as it changes, and
once more as the loader closes.

What may hold a `Persistence` today is a global-scope entity that claims Environmental; the
capture says what it cannot rebuild (a change made outside any action, an action on something
the scene does not make) rather than guessing. Live motion — an open stream, a link — is not
state and does not come back; what it left behind does.

---

## Sub-scripts

`detach` and `run` launch another `.etcs` file with an optional set of bindings:

```etcs
detach forum_lobby.etcs
run    ChessProvider/scripts/chess_service.etcs links=links
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
- **A `spawn` over a name already in scope**, at either scope level. See `spawn` above.
- **Name collisions across the tree** where a local shadows a global of a *different* type. For
  `attach`/`ensure` a same-type shadow is annotated rather than refused: independent scripts
  reaching for an obvious name for an obvious thing is what locals are for. For `spawn` any clash
  is refused, since that verb does not resolve to what is already there.
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
not a broken transcript. A sub-script that `ensure`s the site's server and carries its own
`web.SetPort(8080)` depends on exactly this: against an already-started server on 8443 that is
*supposed* to be refused with a visible line rather than taken or fatal.

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

Two different questions, answered by two different hashes, and they used to be one.

**Which build is this** — the epoch check. Every contract header (`CORE:`, `ONTOLOGY:`) is
SHA-256'd at build time into the module's manifest, and at load the module and the loader each
compare their own copy against the other's: the loader in `Module::validateManifest`, the module
in its own `ETCS_MODULE_EXPORT_MAIN` block. Either side disagreeing refuses the load rather than
risk a silently incompatible call. Under emscripten the module's half is deferred to
`RegisterDynamicLoader` rather than run at static-init — `dlsym` from inside a side module's
static initialiser re-enters the dynamic linker while the load that triggered it is still in
flight, and the page stops with nothing logged — but it still lands before the loader has called
any of that module's tags.

**What is this function** — the region hash, which is what `<Tag>_<Action>_GetHash` now returns.
The leaf is the SHA-256 of the action's own source region: the span from `DEFINE_WORK_FUNC(Tag,
Action)` through the closing brace of the body written under it, computed by `ace hash regions`
and registered into the module's table by the generated `module_hashes.h`. A tag hashes its own
name and each action it declares, in declaration order, over those digests; a module hashes its
name and each tag in the order its Tags string lists them. So an edit to one body moves exactly
one leaf, its tag and its module, and leaves every sibling alone — which is the property a hash
chain over the runtime needs.

The region is SOURCE, not machine code, because a wasm function pointer is a table index: there
is no code address to hash from and no symbol size to hash to, and a chain whose leaves disagree
per platform can only ever compare a build against itself. It is raw bytes, comments included —
the same bargain the header digests make — so a reformat reads as a change. Zero means no region
was recorded for that name (built without `ace` on PATH, or a tag block naming an action no
`DEFINE_WORK_FUNC` defines); it is deliberately not a fallback to hashing the name, because a
name hash returned where a content hash is expected is indistinguishable from one at every
caller.

`GenerateEnvironmentSignature` — name plus the whole manifest — still exists and still answers
the first question. It is no longer what any `_GetHash` export returns.

**Who witnessed these bytes** — the signature, a third question and checked first: `dlopen` runs a
module's static initialisers, so nothing of it may run before it is known. Every module binary
carries a certificate chain (`core/Signature.h`) in a place the platform's loader never maps —
`.etcs.sig` in ELF, an `etcs.sig` custom section in wasm, a trailer after a PE image. Its leaf names
a digest of the bytes the platform does map, under the module's name, and the epochs it is
witnessed in: the epoch is the contract and lib keys of the manifest, hashed together, so libs
count here and stay `[DIFF]` above — two layers of hashing.

The check is a wrap chain, the one the `Wrapper` family already is (`core/Trust.h`). A load is a
frame that refers to the module's bytes; each stage passes it or refuses it, and a refusal is a
failed attach. The loader's own two stages are wired in core, before any module can provide a
type. **Signature**: the chain names these bytes under this name in this epoch, and climbs through
strictly higher roles to a root; passing, it says who signed. **Own**: a module signed by a
loader's key is taken only by that loader, and only in a session where it has proven the key is
its own; what comes to the loader's identity is taken by any loader built with it, and a loader
binary only if it was minted or passed off. Any `Wrapper` child
of the gate's entity stands outside those two, like any chain, and signing is the same chain
wrapped: `ace` signs every module it builds by asking `bin/etcs --sign`, and holds no key. The
bytes checked are the bytes mapped — a sealed memfd on Linux, a file held against writers on
Windows.

A loader is one installation as it runs, and its key is what makes it that one. The key is made on
first use and kept in the OS keystore (`core/Keystore.h`) — the Secret Service, the Credential
Manager — which is the medium `Private` names. A session begins with a handshake: the store's
files say what the loader claims to be, the keystore says whether the claim is its own, and a
loader that cannot prove it takes nothing a loader signed. It does not fall back. A browser holds
no key and is told at link time which loader built it.

A chain is a linked list in two halves, joined at a loader's key. The **local half** is that key
and what it signs: it is all a loader needs to run its own builds, it needs no root, and nothing
but that loader takes it. **ACE's half** is a tree: a super-root that exists only as shards, the
active key it certifies, and under the active key the keys of the loaders that act for ACE — one
role each, sharing that parent. The minting server's loader is `Marketplace` and signs what a
download takes, modules and loader binaries alike; the site's is `Registrar` and certifies other
loaders' keys as `Loader`. `Release` stays in the table and is held by no one. That certificate is the
join: pulled from anticurrententropy.com and installed (`etcs --registration`), it puts ACE's half
above a developer's key, so what that loader signs says whose it is. It still runs only there.
Who may certify whom is a table (`sig::may_issue`), and a key never holds two roles.

**Roots attest each other** the way a registration joins a loader's key to ACE's half: a root
certifying another root's key continues that root's chains past it, and adding to the chain is
trusting it as a remote. An identity may be folded from either root along a chain that carries
the continuation, so one chain runs on loaders of identities from both. A root's own certificate is
self-issued and last; `attest_root.etcs` rebuilds a
root from three custodians and certifies the other root's key, and the other root's role keys
register with that certificate and its root's above their own.

A loader's **identity** is built in: a checksum folded along a chain from a root down to the key
that built it, `h = BLAKE2b("ETCSIDN1")`, then `h = BLAKE2b(h | role | key)` for each key — over keys
and their roles, so reissuing a certificate moves nothing. A builder's identity is the fold along
its own registration to its own key (`etcs --identity`); unregistered, its key alone. It is what
that builder is authorized to build with, and ace passes it to every loader it builds
(`-DETCS_IDENTITY`; `ace identity` builds for another's). It is no header, so it is in no epoch:
every identity's loaders share one, and differ only in what they require. The site's downloads are
built for the minting server's identity, root → active → marketplace, and require that marketplace
at the head of anything they take globally. This is the bootstrap: a loader checking itself proves
nothing to someone who holds no identity, so the first trust comes from where the binary was
obtained, and the identity inside it is what every later check stands on — its own bytes first,
then every module, with an empty store and no key. Such a loader takes what the marketplace signed
and attests what it minted.

A loader trusts what was compiled into it: the identity, and the key of the loader that built it
(`ETCS_BUILDER_KEY`, which ace compiles into every loader), besides its own key proven at the
handshake. A module runs if the fold from one of the roots along its chain comes to the identity,
at whatever key: that is the **global surface** — the marketplace's modules, on a download of its
identity; a registered developer's, on loaders of his own identity or of the root's alone. Short of
that it must meet this loader's own keys — its key, its builder's, a certificate of its
registration — and then runs **local**: no transport bridges its entities
(`MirrorBuffer::localFrame`). A module signed by another loader's key with nothing above it meets
nothing, and is the case refused by default. With no identity built in a loader is
**experimental**: there is no global surface to keep anything off, so nothing it runs is held local
and its links work as they always have — but nothing places it among anyone else's, which is what
the expanding multiplayer needs. A
builder's key compiled in is also what lets a loader run with no keystore at all: `ace bundle` puts
`bin/etcs` and a few modules on a machine with nothing else, and they load.

The root is made by etcs itself, on such a machine: `root_ceremony.etcs` makes the super-root, the
active key it certifies, and the marketplace, release and registrar certificates the active key
issues (the release certificate is held by no one now), splits both keys 3 of 5 to five custodians, writes the certificates out, and forgets both
keys. `certify.etcs` rebuilds the active key from any three custodians to certify another key later.
Once the root is in a registration, what is handed on is the loader's own key, its registration,
and its lineage.

What the ceremony runs is agreed on before it runs. **Witnesses**: `ace source` packs this tree,
Commons and ACE-Build-Tools, each with its `.git`, and the Monocypher pin into one tar a machine
with no network builds from. On each machine, unpacked at the same path (it is compiled into the
loader and some modules), `ace witness` builds the loader and the ceremony's modules twice from
scratch with no builder key. The loader just built then writes a statement, `etcs --witness`, and
only if both builds came to the same digests: the digest of each binary, the epoch, the arch, and
the builder's account of what it built from (the commits, the path, the compiler), signed by a key
made for it and forgotten. `etcs --consensus` agrees on statements: one epoch and one source across
all of them; per arch, every witness the same digests, from at least a quorum of distinct
witnesses. Where two disagree it names what their accounts differ in. With `check=<dir>` it says
whether a directory's binaries are the consensus build, and `ace bundle --from` packs that build for
the ceremony. **Minting** is the same agreement with the marketplace key at the end: witnesses
build for the minting server's identity (`ace witness --identity`), and `ace mint` on the minting
server checks the consensus of the build it is handed, refuses one built for another identity, and
signs those exact bytes, the loader among them, for `ace publish`. Builders of lower trust run the
build before the higher key goes on it. Binaries are compared by the digest a loader checks (`etcs --digest`), never as
files, since every signer's section differs.

The site is where the two halves meet. It serves **ACE's half on one path** (`/pki`): its registrar
is a `CryptProvider::Registrar` route whose key is a `Keypair` kept in the site's keystore, and a
loader that signs `"ETCSREG1" | its key | the registrar's key` gets a `Loader` certificate and the
chain above it. It serves **the binaries on another** (`/dist`): what was minted, modules and
loader, with a list of their digests and epochs and the epoch the site is in, signed by the marketplace. A
loader reads that list through `CryptProvider::Upstream` and fetches whatever the site has moved
past, unless its own key signed it, in which case it belongs to the developer. A module never
holds the loader's key or writes its store: everything that does is the loader's own code, reached
through a `TrustEvent` on the loader's ordering thread. A fetched binary is put in place only if it
is the bytes the list names and passes the loader's own stages in the site's epoch, and it is the
next session's. The list names the ABI's version it was published under (`BASE_SOURCE_STRING`, which
moves on an ABI change and so with every change in trust a release makes), and a loader takes no
list older than itself or than the newest it has taken, which its store keeps: a list replayed from
before a key was retired is refused. Retiring the marketplace key is a new identity, with the next
key at the third link, so the loaders built with it take nothing the retired key signs. The same version is on every function a module or loader exports: the version script's
node is named for it (`Foo_Make@@ETCS_ABI_KERNEL_V_1.0`), so each symbol says at its source which
kernel it was built for.

**Re-sync** is what a retired marketplace key leaves behind. The loaders built with the old identity
believe only that identity's lists, so they are also told its parent — the fold one step short of
the marketplace key, root → active (`-DETCS_IDENTITY_PARENT`). A checksum is followed only down a
chain, from keys to a head, never back up, which is where it can be checked at all; so the parent
is given to the loader rather than found. A list from another marketplace that the same parent
certified, at a newer kernel, is the identity the loader moves to, and it gives the loader one
thing: its successor, minted by that marketplace and countersigned by a release key under the same
active key — two keys, neither of them the retired one. The loader puts it in place, retires its old
identity in its store, so nothing the old marketplace signs is taken again whatever kernel it
claims, and restarts on the successor where the session left off: the scene replays as each
persistable type defines, and the resume compares state hashes only within one kernel. A successor
that never took is not fetched again; the loader says to reinstall from the site.

A loader is the one binary nothing else checks, so it attests itself: at the handshake it puts its
own bytes through its own two stages. That is what makes a **lineage**. The loader going out
**passes off** the one that replaces it: after the chain in the child's signature section it signs
`"ETCSGEN1"` | parent digest | child digest | the epoch on either side | its identity | whether it
attested | its key. That is the one fragment a loader can sign for; the lineage behind it is the
marketplace's to attest, at the site. A loader binary its own key (or its builder's) signed attests
only if an attesting loader of the same identity passed it off. A loader that passed itself off is
a lineage beginning, which a loader of no identity, or of its own key's, takes, and one of a minted
identity does not: there a lineage begins at a mint, whose signature attests the binary by itself,
and the build after it is passed off by the minted loader. A loader that does not attest still
runs, with a warning, and signs — its local half alone, since local authority is any loader's and
global authority a signed build's. What it already signed with its half is left as it is.
`lineage.log` in the store is the signer's own trace of what it passed off: parent, child, epochs,
identity, and its word for when and where.

`DatabaseProvider::Keystore` is that same store as a `Database` claiming `Private`: where a script
keeps a secret, and the one place a `Keypair` goes whole. Its `Wrapper` children guard it as they
would a node on a link, so a secret kept under a `CryptProvider::Envelope` — a stage that seals
under the key its own `Private` child holds, given by no verb — is reached only by an entity
carrying the same one. Naming it is not enough.

Unbuilt: the minting server's identity, which waits on the ceremony being run for real (a
rehearsal's root, or a test authority's, stands in); the unlock of a downloaded loader's head gate,
which would let it take chains that do not start at the marketplace. A link that carries a local module's entities is refused (the
local frame); nothing yet resolves one epoch against another. An account behind a registration: the registrar certifies any key
that proves itself, and binds it to no one. Also unbuilt: the marketplace's attestation of
the whole lineage, beside the download; a successor from a later epoch checked by the lineage (it is
checked by the marketplace's signature, in the epoch the site's list names); signing and attesting
the web loader, whose trust is its page's connection; the
entitlement stage; and the stage that takes another loader's modules (the untrusted scanning
mode).

## Shutdown

Loader unload and the root's own vacate are ordered so a module is never unloaded while a call
into it could still be pending — see the loader-exit-path material for the mechanism.

A module is held by its **roots**: its global-scope entities, and any child one of its types
became under another module's entity (a `Persistence` under a `Ledger`). Such a child is a root of
its own module — it lives in that module's arena, not its parent's, and holds the module's
lifetime exactly as a global entity would, handing it to a sibling root when it goes. Only its
place in the graph is its parent's: it leaves with its parent, through its own module.

Before any of that, the loader says it is closing: what keeps the scene saves it one last time
and then writes nothing more, so a half torn-down runtime is never what the next start resumes.

---

## Summary

| Concept | Mechanism |
|---|---|
| Required input | `requires <name> [Tags]` — name in the address slot, type constraints in the bracket; resolves through the same closure lookup as `attach`, checked before the script runs |
| New entity | `spawn Module::Tag <name>` — always creates; refused if the name is taken |
| Closure entity, strictly | `attach Module::Tag <name>` — binds an existing one, refuses otherwise |
| Closure entity or new | `ensure Module::Tag <name>` — binds the closure's, else creates |
| New typed child | `<parent>.spawn( Module::Tag <name> )` |
| This parent's child, strictly | `<parent>.attach( Module::Tag <name> )` — parent is part of the test |
| This parent's child, or new | `<parent>.ensure( Module::Tag <name> )` |
| Action | `<name>.Action(payload)` — receiver and brackets both mandatory |
| RID in a payload | `@name` |
| What a RID is | module, type and order of making, hashed — the reference inside a run, in no record, hash or store |
| Stream | `<a>.Produce(payload) -> <b>.Consume(payload)` — one line, both ends |
| Entity in another runtime | a local instance of its type with a `NetworkProvider::Remote` child, bound to a published name — its actions and stream halves run there, on one link per pair of runtimes, through the node's `Wrapper` authority layer |
| Keeping a scene | a `DatabaseProvider::Persistence` child on a global `Environmental` entity — its actions, compacted to a script, plus its values; offered back at the next start (`--resume` / `--fresh`) |
| The frame edge | `view.Compose(@root)` and one detached `RunFrames` — every `Animated` leaf visited once a tick; a visit may change the graph, a walk under the present's hold only reads |
| Interrupting work | `<name>.kill(<label> [index])` |
| Removing a flag | `<name>.unflag(<flag>)` — the mutable set; a `requires` tag is fixed and out of reach |
| Scope | local closure + the root script's names as globals; no ancestor chain |
| Passing a local down | `detach child.etcs k=name` / `run child.etcs k=name` |
| Comments | `#` prefix |
| Sub-script search path | `#IMPORT` / `#EXPORT` on line two |
| Module signature | automatic at module load, before anything of the module runs |
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

- **`spawn` refuses a name already in scope**, and the parser honours backslash escapes inside
  quoted spans, matching `TBuffer`'s own extractor. Both close gaps where two layers disagreed
  about the same text: one about what a name meant, one about where a payload ended.

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