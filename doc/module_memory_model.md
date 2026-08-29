# The Module Memory Model

How Module lifetime tokens are held, transferred, and released across
Root, global-scope Entity, and child Entity — and what actually happens,
mechanically, when each of those is deleted.

---

## Summary

Three distinct kinds of thing can hold a Module's lifetime token, and
each has a different relationship to memory:

- **Root** — standalone, does *not* derive from `Entity`. No arena of its
  own, no typed children, no tags. Typically stack-allocated. Exists
  purely to give `attachModule` something to bootstrap against before any
  real entity of the requested type exists yet.
- **Global-scope Entity** — `parent_ == nullptr`. Spawned top-level
  (`ModuleBundle::operator()()`), never via `addTag<T>`. Its own
  `local_arena_` is allocated from the **global** `MemoryArena` singleton.
  Eligible to become a Module's `lifetime_owner`.
- **Child Entity** — `parent_ != nullptr`. Spawned via a parent's
  `addTag<T>()`. Its entire footprint — outer object *and* its own
  `local_arena_` — lives inside its **parent's** arena, not the global
  singleton. Never itself a `lifetime_owner` candidate.

Deletion happens through exactly two *explicit* machinery paths, both of
which converge on the same underlying call — `MemoryArena::deleteEntity`
— and one *implicit*, ordinary-C++-`delete` path that is only fully safe
for the child case as currently written:

| Path | Trigger | Runs `registerDtor<T>`'s callback? | Safe today? |
|---|---|---|---|
| **Ordinary `delete` (child)** | any code calling `delete childPtr` | No — destructor already ran via normal C++ semantics; `operator delete` just unlinks the now-stale record | Yes |
| **Ordinary `delete` (root-scope)** | any code calling `delete rootScopedPtr` | No — `operator delete` has no root branch at all | **Not exercised anywhere in this codebase**; would leave a dangling `DestructorRecord` |
| **`EntityUnloadEvent`** | explicit external caller targeting a *child* (e.g. `removeTag`'s entity-relation case) | Yes | Yes |
| **`DestroyEvent`** | a leaf firing its own destruction (e.g. `SocketConnectionState::DeleteConcrete`, `GLFWWindow::closeWindowConcrete`) | Yes, via `destroyImpl`'s own call to `deleteEntity` | Yes — this is the path actually observed doing real work |

A global-scope entity's death and its module's unload are **two separate
events, temporally apart**: the entity dies immediately; the module only
unloads after `RequestUnloadEvent`'s 600ms grace window finds nothing
reclaimed `lifetime_owner` in the meantime.

---

## 1. The Three Roles

### Root

Defined standalone at the bottom of `Entity.h`, deliberately *not*
inheriting `Entity`. A Root's entire purpose is to host a `Module` member
until it either hands the lifetime token to something real, or goes out
of scope still holding it. It has no `local_arena_`, no `tags`/`flags_`/
`typed_children_` — there was never anything for those to back, since
Root never has typed children and is never dispatched through any tag.

Root previously *did* inherit `Entity`, purely to satisfy `attachModule`'s
parameter type. That cost every Root an entity-local sub-arena it never
used — since `~Entity()`'s body never reclaimed it early, and Root was
never arena-resident for anything else to reclaim it via, every Root ever
constructed leaked one 4KB `MemoryArena` until process exit. Root no
longer inheriting `Entity` at all is what closes that leak structurally.

`Root`'s tag is deliberately lowercase (`"root"`) — the one exception to
the TitleCase convention every real ontology tag follows, marking it as
structurally distinct rather than merely differently-named.

**Why "root" names one role at two layers, not two coincidentally-named
things.** `Root` (the class) and a root-scope `Entity`
(`parent_ == nullptr`, below) share a name because they are the same
functional role, occupied at two different points in a module's
lifecycle. `Root` plays that role entirely from *outside* any module —
the thing handed to `attachModule` to bootstrap a vacant module before
any real, typed entity of it exists yet. A root-scope `Entity` plays the
identical role *after* the module has real entities: the top of the
local ownership hierarchy, and — critically — the thing eligible to
inherit the lifetime token directly from a `Root` the moment it attaches
(`attachModule`'s explicit Root→Entity hand-off, §3). That token
transfer is what makes the shared name exact rather than coincidental: a
root-scope entity doesn't merely resemble Root's former job, it
literally takes over the identical responsibility — holding
`lifetime_owner` for the module — the instant it exists. `Root` is
needed exactly once per module: at the single entry point where that
module has to come into existence from nothing. Once it's anchored,
every further entity is created directly against the live anchor — as a
child via `addTag<T>`, or as another module-global (root-scope) entity
of the same or a different tag — with no `Root` involved at all. `Root`
isn't merely unused after bootstrap; it is structurally absent from the
rest of the module's life.

### Global-Scope Entity

A genuine `Entity`-derived leaf, spawned through the top-level path
(`ModuleBundle::operator()()`, which is what `_make_##Name` ultimately
routes through). `parent_` is `nullptr` from construction — nothing ever
set it, since that field is only ever assigned inside
`addTagTrampoline<T>()`.

Its constructor allocates `local_arena_` here:

```cpp
local_arena_((s_pending_parent_arena_ ? *s_pending_parent_arena_
                                       : MemoryArena::getInstance())
      .allocate<MemoryArena>(DEFAULT_ARENA_START_PAGE, false))
```

Since nothing has set the thread-local `s_pending_parent_arena_` at
top-level spawn time, this resolves to `MemoryArena::getInstance()` — the
**global** singleton. This is the only kind of entity eligible to become
a Module's `lifetime_owner` (see §3).

### Child Entity

Spawned via `Entity::addTag<T>()`. Before construction, `addTag<T>`
temporarily points the thread-local `s_pending_parent_arena_` at the
*parent's own* `getArena()`:

```cpp
MemoryArena* saved = s_pending_parent_arena_;
s_pending_parent_arena_ = &getArena();
T* child = getArena().allocate<T>(std::forward<Args>(args)...);
s_pending_parent_arena_ = saved;
```

Because the child's own constructor runs while this is set, its
`local_arena_` resolves to the *parent's* arena rather than the global
singleton — meaning a child's entire footprint (outer object **and** its
own `local_arena_`) lives inside its parent's arena tree. This was a real
bug fix: an earlier version always drew from the global singleton
regardless of parentage, meaning a parent's own arena teardown could
never reach or cascade-clean its children's memory at all.

`addTagTrampoline<T>()` (run on the loader's ordering thread, via
`AddTagEvent`) is what sets `child->parent_` and `child->parent_rid_`
afterward, and inserts the child into the parent's `typed_children_` map
— a child entity is never eligible to become a `lifetime_owner`.

---

## 2. Arena Inheritance, Concretely

For a **global-scope** entity, two separate allocations sit on the same
parent (the global singleton)'s `dtorHead_` chain: the entity itself, and
its own `local_arena_` (a `MemoryArena`, itself non-trivially
destructible, itself registered via `registerDtor<MemoryArena>` — the
plain, non-`Entity` branch, since `MemoryArena` doesn't derive from
`Entity`). These are two independent records, not nested — which is
exactly why the deletion callback for the global-scope case (§4) has to
evoke both explicitly, one after the other.

For a **child**, both the child itself and its own `local_arena_` are
allocated *from* the parent's arena — so they live as records on the
*parent's* chain, not the global one. Reaching into the parent's arena
(a reset, or a full ancestor-level teardown) reaches everything beneath
it in one sweep; nothing about a child's own existence ever touches the
global arena's chain at all.

---

## 3. The Module Lifetime Token

`Module` (`Bundles.h`) has:

- `LifetimeOwner lifetime_owner` — **global-instance-only.** Which
  entity or Root currently holds the token. Default (`Kind::None`) means
  vacant.
- `bool is_lifetime_owner` — **per-token-only.** True iff *this*
  particular Entity's or Root's own `module_` is the one `lifetime_owner`
  points at, on the global instance its own `parent` resolves to.

`LifetimeOwner` is a tagged union (`Kind::None | Entity | Root`) rather
than a bare `Entity*`, specifically because a Root is exactly as valid a
token-holder as a real entity, and the two are no longer related by
inheritance. Both constructors from a raw pointer check for null
explicitly and collapse to `Kind::None` — this is load-bearing, not
style: a `LifetimeOwner` tagged `Kind::Entity` with a null pointer behind
it would report itself `bool`-truthy while holding nothing, which is
exactly the bug that surfaced the first time a survivor search
legitimately found no sibling and passed a raw `nullptr` straight into
`promoteOrVacate` before this fix.

Claiming the token happens in exactly one place, `attachModule`: the
first entity or Root to attach to a vacant module claims it
unconditionally; if the current holder is a bootstrap `Root` and a real
`Entity` attaches afterward, the token transfers explicitly (a Root was
never a genuinely dispatched type, so it never keeps precedence over a
real one).

---

## 4. Deletion Paths, In Order of How Much Machinery They Touch

### 4a. Ordinary `delete` — child case

```cpp
void operator delete(void* ptr)
{
    Entity* e = static_cast<Entity*>(ptr);
    if (e->parent_ != nullptr)
        e->parent_->getArena().forget(e);
}
```

For a child, the destructor has *already fully run* by the time
`operator delete` executes — that's ordinary C++ delete-expression
semantics, not anything this codebase controls. `forget()` unlinks the
now-stale `DestructorRecord` from the parent's own chain *without*
calling its destructor again — the record must be removed or the
parent's own eventual arena sweep would call the already-run destructor
a second time (undefined behavior, and not merely theoretical — several
leaf destructors in this codebase are not idempotent).

### 4b. Ordinary `delete` — root-scope case

The same function, for `e->parent_ == nullptr`, does **nothing at all**.
No branch executes. This path is not exercised anywhere in the current
codebase — every global-scope entity observed so far self-terminates
through 4d, never through a bare `delete`. If something *did* call
`delete` directly on a global-scope entity, `~Entity()` would run
correctly (ordinary destruction), but the entity's own `DestructorRecord`
would remain on the global arena's chain, uncalled and pointing at
already-destructed memory, until that chain is eventually walked — at
which point its destructor would be invoked a **second** time. This is
flagged as an open question in §10, not treated as a live bug — nothing
currently takes this path.

### 4c. `EntityUnloadEvent` → `entityUnloadImpl` — explicit child target

The documented general-purpose explicit-deletion event. Its own header
comment describes two trigger sources: `operator delete` firing it
unconditionally for roots (see §10 — this branch does not currently
exist in `operator delete`'s actual body), and an explicit external
caller targeting a child directly. The second is real and exercised: for
example `Entity::tagModifyImpl`'s `removeTag` path, when the tag being
removed points to an entity relation (`TagEntry::child` set) rather than
a bare dispatch entry — the tag-list adjustment happens locally, then
`EntityUnloadEvent{child_to_delete}()` fires outside the lock to do the
actual deletion.

`entityUnloadImpl` determines the target's correct parent arena
(`getParent()->getArena()` for a child; `module_.parent->module_arena`
for a global-scope entity) and delegates to `MemoryArena::deleteEntity`.

### 4d. `DestroyEvent` → `destroyImpl` — self-terminating leaf

The path actually observed doing real work. A leaf fires this on itself
as part of its own native cleanup — `SocketConnectionState::DeleteConcrete`
and `GLFWWindow::closeWindowConcrete` both follow this shape:

```cpp
ETCS::DestroyEvent{conjugate_key.c_str(), getRID()}();
```

`destroyImpl` removes the RID from the type's `RIDListHandle` tracking
*and* — fetching the live `Entity*` before doing so — delegates to
`MemoryArena::deleteEntity` on the correct parent arena, exactly the same
determination `entityUnloadImpl` makes. This is a second, independent
entry point into the same underlying deletion machinery as 4c, not a
lighter-weight alternative to it.

---

## 5. What `deleteEntity` Actually Runs

`MemoryArena::deleteEntity(target)` looks up (without unlinking) the
target's `DestructorRecord.run_entity_delete` callback and invokes it.
That callback is built once per concrete type `T`, inside
`registerDtor<T>`, specialized at compile time via
`if constexpr (std::is_base_of<Entity, T>::value)` — so `MemoryArena.h`
itself never needs `Entity`'s full definition, only a forward
declaration.

**Global scope** (`e->getParent() == nullptr`):

```cpp
Entity* survivor = parentArena.findNextCandidateScope(
    [](Entity*) { return true; }, e);
e->module_.promoteOrVacate(survivor);
parentArena.evokeDestructor(e);
parentArena.evokeDestructor(&e->getArena());
```

Searches the *same* parent arena's chain for any other live root-level
entity (a genuine sibling — never one of `e`'s own children, since those
are never candidates and are evoked unconditionally regardless). Calls
`promoteOrVacate` with whatever it finds (§6), then evokes **both**
records described in §2 — the entity and its own arena — since they are
independent entries on the same chain.

**Child** (`e->getParent() != nullptr`):

```cpp
e->reparentChildrenTo(e->getParent());
parentArena.evokeDestructor(e);
```

No election is possible — only root-level entities are ever
`lifetime_owner`-eligible. `reparentChildrenTo` walks the dying entity's
own `typed_children_` and re-points every grandchild's `parent_` up one
generation, skipping the dying entity entirely rather than orphaning
them. Only the entity's own outer record is evoked — **its own
`local_arena_` is deliberately left intact.** This is the "coyote time"
property: everything the dying entity itself had `addTag`'d remains
fully live and reachable through the (now-reparented) chain, until
something explicitly reaches further in — a targeted `EntityUnloadEvent`
on it or an ancestor, or an eventual cascade when some ancestor's own
arena is reset or torn down.

---

## 6. `promoteOrVacate` and the Two-Phase Teardown

```cpp
void Module::promoteOrVacate(LifetimeOwner survivor)
{
    if (!is_lifetime_owner) return;
    ...
    if (survivor) { /* transfer the token to survivor */ return; }
    global->lifetime_owner = nullptr;
    ETCS::RequestUnloadEvent{global->name, global}();
}
```

A no-op if this particular token was never the elected owner — the
common case, since most global-scope entities dying are ordinary proxies
rather than the current holder. If a survivor exists, the token transfers
immediately, synchronously. If not, `lifetime_owner` is cleared and
`RequestUnloadEvent` fires — **non-blocking.** The loader spawns a
detached thread that sleeps 600ms, then re-enqueues the same event with
a recheck flag set. `requestUnloadImpl` only then re-verifies
`lifetime_owner` is *still* vacant (a fresh spawn from the same module
during the delay would have reclaimed it, making this a no-op) before
actually erasing the module's registry entries and calling
`cleanupModule()`/`dlclose()`.

This is why an entity's death and its module's unload are observably two
separate events: the entity — and its own local arena — are fully torn
down synchronously, inside the same call that fired `deleteEntity`. The
module itself, if nothing reclaims it, only actually unloads roughly
600ms later, asynchronously, on the loader's own timer.

---

## 7. Root's Parallel Path

Root never goes through `MemoryArena::allocate<T>()` the way a real
entity does, so `registerDtor<T>`'s Entity-branch machinery never applies
to it at all — Root's teardown is entirely ordinary C++ destruction.
`~Module()`'s second branch is what root-scope teardown actually reaches:

```cpp
else if (is_lifetime_owner && parent)
{
    ETCS::ChangeModuleEvent{"", &hosting_entity.asRoot()}();
}
```

This branch is provably reachable **only** for a Root-hosted token.
`module_` is `Entity`'s *first* declared member, meaning — by ordinary
C++ construction/destruction order — it is constructed first and
destroyed **last**, strictly after `~Entity()`'s own body has already
completed. But `~Entity()` only ever runs via `evokeDestructor`, which
only ever fires from *inside* the global-scope `registerDtor<T>`
callback (§5) — and that callback calls `promoteOrVacate` (clearing
`is_lifetime_owner`) **before** evoking the entity. So by the time an
Entity-hosted `module_` ever reaches its own destructor, `is_lifetime_owner`
has already been reset by the very callback that's the only thing capable
of triggering this destructor chain in the first place. Root has no
equivalent gate — it destructs "for free" on ordinary scope exit, with
nothing forcing a `promoteOrVacate` call first unless
`Root::changeModule()` happened to run beforehand. This branch exists
specifically to catch a Root that fell out of scope holding the token
without ever calling that.

`ChangeModuleEvent` with an empty target routes to `changeModuleImpl`
(relinquish-only mode), which searches `root_registry` — the Root-specific
counterpart to the arena-chain search in §5, since Root never appears in
any arena's dtor chain to be found that way — for another live Root
already attached to the same module, calls `promoteOrVacate` with
whatever it finds (or doesn't), and unregisters this Root before clearing
its own token fields.

---

## 8. Ordering and Synchronization

Three graduated tiers of causal weight, not a flat local/global split:

- **`TagModify`** — mask is `1 << <this entity's own type bit>`
  (`GetTagBit`). Same-type operations serialize against each other via
  the reorder buffer's collision logic; different types commit
  independently. Never forwarded to the loader at all — resolved locally
  within the emitting entity's own module.
- **`Resolve` / `Destroy` / `AddTag`** — mask is `1 << <the module's own
  bit>` (`GetModuleBit`). Serializes against other operations touching
  the *same module*; independent of unrelated modules.
- **`Load` / `EntityUnload` / `ChangeModule` / `RequestUnload`** — mask
  is `~0ULL`. These can restructure the shared topology itself (a module
  going from vacant to anchored, an arena being reclaimed, a lifetime
  token changing hands) — never treated as commutable with *anything*
  in flight, regardless of type or module.

The `Ack` mechanism (`DLInEvent::reply_to`, `sendAckIfNeeded`) provides
the happen-before edge across the module/loader thread boundary: after
finishing one of the five reply-eligible kinds, the loader enqueues a
lightweight ack onto the *originating* module's own ordering thread and
blocks on it — guaranteeing the calling module has fully processed a
sync point before its own blocking call returns. If the target stream is
already mid-shutdown, the enqueue fails fast and the loader proceeds
without waiting for an ack that would never arrive — a deliberate,
graceful degradation, not an error case.

This is a genuine, well-motivated hierarchy — causal weight scales with
how much of the shared graph an operation can actually touch. It is not,
however, a purely decentralized "no privileged frame" design: for the
topology-changing tier specifically, there is a single serializing
authority (the loader's one ordering thread), which is closer to a
privileged coordinator than a system built entirely from independent
local decoherence fronts. Some coordinator for structural changes is
close to unavoidable; the more precise description of this system is
*local where locality is sound, single-authority where the operation is
structural* — the three-tier mask granularity is the sharper engineering
claim, not the pure-decentralization framing on its own.

---

## 9. Open Architectural Questions

**`operator delete`'s missing root branch.** `EntityUnloadEvent`'s own
header comment states it is fired "unconditionally for roots" from
`Entity::operator delete`. The actual body of `operator delete` has no
such branch — it is a no-op whenever `parent_ == nullptr`. Nothing
observed so far exercises this path; every global-scope entity so far
self-terminates through `DestroyEvent` (§4d) instead. Two readings are
both consistent with everything else in this codebase: either the
comment describes an earlier design that a refactor superseded (the same
pattern already self-documented elsewhere — see `registerDtor<T>`'s own
comment acknowledging a similar stale reference), making this branch and
that half of the comment dead weight safe to remove; or a root-scope
`delete this` is meant to be a supported alternative path that, as
currently written, would not correctly vacate `lifetime_owner` at all —
a latent gap rather than a currently-triggered bug. Which of these is
intended determines whether `operator delete` needs a root branch added,
or `EntityUnloadEvent`'s comment simply needs correcting to describe
`DestroyEvent`/`entityUnloadImpl`'s explicit-child-target case as the
sole real entry point.

**`RequestUnloadEvent`'s own header comment** attributes its firing to
`Module::~Module()`. The observed and traced call site is actually
`promoteOrVacate` (§6), called from inside `registerDtor<T>`'s callback —
`~Module()` never fires it directly anywhere in its own body. A second,
minor instance of the same comment-drift pattern above.
