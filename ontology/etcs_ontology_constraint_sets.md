# ETCS Ontology: Constraint Sets, Families, and Incidental Exclusivity

*A description of the structural principles underlying `ontology.h`'s `xxx.h` /
`xxxBase.h` convention — as design intent, not as a record of what is
currently mechanically enforced in code.*

---

## 1. The two-file convention as a constraint declaration

Per `shape.txt`:

```
xxx.h     = definition of an interface
xxxBase.h = definition of a concrete type (leaf CRTP)
```

An `xxx.h` interface (e.g. `HtmlPage_`, `ConnectionState_`, `Parser_`) declares
a set of pure virtual methods. An `xxxBase.h` CRTP template (e.g.
`HtmlPageBase<Derived>`, `ConnectionStateBase<Derived>`) is a proxy layer: it
declares the corresponding `*Concrete` methods a leaf type must implement, and
dispatches the interface's virtual calls to them via `static_cast<Derived*>`.

This is not merely a code-organization pattern. Each `xxx.h` interface is a
**constraint** — a named requirement that a type must satisfy a specific set
of behaviors to legitimately claim that identity. The `Base` template is the
mechanism by which a concrete leaf *proves* it satisfies that constraint, at
compile time, via the CRTP dispatch.

Every type in the ontology, at its root, virtually inherits `ETCS::Entity`.
This universal shared root is what makes combining constraints from
*different* families safe: because the inheritance is virtual, every path
back to `Entity` collapses into a single shared subobject, regardless of how
many families a leaf type draws from.

---

## 2. Families as constraint sets, and structural exclusivity

An inheritance lineage in the ontology —`Entity → X → {Y, Z}`, for
example — defines a **family**. Membership in a family is *cumulative* down
the lineage and *exclusive* across siblings at the same branch point:

- **Cumulative**: if `Y` inherits from `X` and adds further virtual methods,
  `Y`'s constraint set is the union of `X`'s methods and `Y`'s own additions.
  `Y`'s `Base<Derived>` CRTP proxy must therefore satisfy the *entire*
  accumulated set walking back to `Entity`, not merely the increment `Y`
  itself introduces. Refinement adds obligation; it never subtracts it.

- **Exclusive across siblings**: `Y` and `Z`, both descending from the same
  `X`, are two different specializations of the *same* constraint. A leaf
  cannot legitimately claim to be both — not because of a C++ diamond
  problem (virtual inheritance from the shared `Entity` root already
  prevents that class of issue), but because holding both simultaneously
  would be an incoherent claim about what the entity *is*. This is a
  semantic rule about family membership, not merely a technical one about
  ambiguous method resolution.

### Where a lineage is declared — two spellings, one meaning

A refinement can be stated in either of two places, and both are load-bearing:

```
class LocalDatabase_ : public Database_          on the INTERFACE
DrawableBase : public SurfaceBase<Derived>       on the BASE
```

The second is not a shorthand for the first; for most of the ontology it is
the only spelling available. `ETCS_SUPERTYPE_BASE` inherits its interface
**non-virtually**, so an interface that also inherited its parent interface
would be reached twice — once through the interface, once through the composed
Base — and every inherited call would be ambiguous. The rule the ontology
settled on, stated in `Drawable.h`, is therefore:

> an interface declares only its own **increment**,
> a Base carries the **lineage**.

`Database` can use the interface spelling because it has no `DatabaseBase.h`
at all — nothing composes it, so nothing reaches it twice. Every family that
*does* have a Base uses the second spelling. The entire `Resizable → Surface →
Drawable → {Drawable2D, Drawable3D} → Camera` lineage is declared this way, and
a reader (or a tool) that inspects only interface inheritance sees six
unrelated families sitting flat beside each other.

That second spelling is also where the **exclusivity** lives, and where it is
mechanically enforced. Because `Drawable2DBase` and `Drawable3DBase` both
compose `DrawableBase` non-virtually, a leaf inheriting both acquires two
`Drawable_` subobjects and every call through them is ambiguous — the
incoherent claim is a compile error rather than something a reviewer has to
catch. `ace ontology` reads both spellings and prints the resulting forest,
with the exclusive sets named and every module's leaves audited against them.

Using bracket notation for illustration: given families rooted at `X`, `A`,
`C`, and `D` respectively, a leaf type may draw **at most one member from
each family**:

```
[X, Y, Z]   [A, B]   [C]   [D]
```

A valid leaf might compose one pick from each bracket — e.g. `Y + B + C + D`
— but never two picks from the same bracket (`Y + Z` is invalid).

---

## 3. Orthogonal composition via fold-in at the `Base` layer

Distinct from sibling exclusivity within one family is **deliberate
composition across independent families**. When a leaf's `Base<Derived>`
template inherits from more than one family's `Base`, that is a single,
coherent choice: "one pick from the domain-role family, bundled with one pick
from the lifecycle family." This is not exclusivity being violated — it's
exclusivity being correctly scoped.

Note the tension with the previous section: composition at the `Base` layer is
how refinement is declared **and** how orthogonal axes are folded in, so the
spelling alone does not distinguish them. What distinguishes them is whether
the composing family's own header claims the relationship as an *is-a*.
`Drawable.h` does — "This is a refinement of Surface, not a sibling of it.
Every Drawable is a Surface" — and `SurfaceBase` composing `ResizableBase` and
`OrderableBase` is the same claim made twice (every surface has a size and a
stacking position). A family may therefore refine more than one parent; `ace
ontology` renders it under the first and cross-references it under the rest.

A leaf-level fold-in of two unrelated axes is a different act from a
family-level refinement even though both are written `public XBase<Derived>`,
and the difference is in the interface header's own claim, not in the syntax.
Exclusivity applies *within* a family (siblings), never *across* independent
families (orthogonal axes). Folding two orthogonal families together at the
`Base` declaration is the intended mechanism for building a leaf's full,
specific constraint set from multiple independent axes at once.

---

## 4. Incidental exclusivity: constraint sets discovered, not designed

Families as described above are **structural** — the exclusivity exists
because of an explicit, authored ancestor relationship in the type hierarchy,
fixed before any leaf type is ever written.

A second, different kind of exclusivity can arise **nominally**, purely by
accident of naming, between families that share no ancestor and were never
considered relative to one another:

- Suppose family `C` and family `D` are entirely unrelated — no common
  ancestor, no intentional relationship.
- Suppose both happen to declare a method with the same name and signature
  (e.g. both declare `Reset()`), each for its own, conceptually distinct
  reason — `C`'s `Reset()` might describe resetting a data-flow position,
  `D`'s might describe resetting persistent state. The two meanings are
  genuinely different; the shared word is coincidence, not evidence of a
  shared constraint.
- The moment a leaf attempts to fold **both** `CBase<Derived>` and
  `DBase<Derived>` into its own `Base` declaration, an unqualified call to
  `Reset()` becomes **ambiguous** — a hard compile error.

This is the critical distinction from ordinary sibling exclusivity: `C` and
`D` were *not* exclusive as families in the abstract. They only became
mutually exclusive **at the specific moment someone tried to combine them**,
and only because of an incidental name collision neither family's author
necessarily anticipated. In effect, the type system discovers a previously
unconsidered relationship between two axes of the ontology, purely as a side
effect of vocabulary overlap:

```
[X, Y, Z]   [A, B]   [C, D]   ← C and D were never designed as a pair;
                                 the pairing is discovered lazily, only
                                 when someone actually tries C + D together
```

This is meaningfully different from a true structural family:

- It is **pairwise and incidental**, not partition-wide. `C` may combine
  freely with some unrelated family `E`; `D` may also combine freely with
  `E`. Only `C + D` specifically collides. If a third family `F` later also
  happens to declare `Reset()`, there is no guarantee `C`, `D`, and `F` form
  one coherent triple-exclusive group — the actual conflicts among them are
  whatever pairs have genuinely been attempted, not a designed set.
- It is **discovered lazily**, one attempted combination at a time. Whether
  `C` collides with some distant, as-yet-uncombined family is unknown until
  someone actually tries to fold both into the same leaf.
- It is **escapable**, unlike true sibling exclusivity, via explicit
  disambiguation (see §5) — it defaults to a compile error, not a permanent
  prohibition.

### Naming as a constraint-authoring act

Because incidental exclusivity is triggered purely by name (and signature)
overlap, the choice of method names across independent families is not
cosmetic. Reusing a common word (`Reset`, `Close`, `Init`) across two
unrelated interfaces implicitly opts those two families into "must reconcile
if a leaf ever tries to combine them." Deliberately varying the name
(`Reset` vs. `ResetFlow` vs. `Rewind`) is how an author keeps two axes freely
and silently combinable forever. This makes the ontology's vocabulary itself
part of its constraint surface — a fact worth documenting alongside the
`xxx.h` / `xxxBase.h` convention, since it is not visible from reading either
family's interface in isolation.

---

## 5. Resolving a genuine collision: disambiguation, not defaults

When two orthogonal families collide on a name, the compiler forces a choice
at the point of combination — it will not silently pick one family's
implementation over the other's, nor will it silently let one leaf method
satisfy both unrelated pure virtual slots, *unless* the leaf author
explicitly writes one function that does so. This is itself a property of
the ontology worth naming directly: **the moment of disambiguation is
mandatory, and doing that disambiguation explicitly and correctly is an
exclusive property of this design** — the model does not merely tolerate the
collision, it requires the author to resolve it, on the spot, before the
type can exist at all.

There are two legitimate resolutions:

1. **Explicit merge.** If both constraints are genuinely meant to hold
   simultaneously for this leaf, the author writes a single combining
   function that calls each family's original implementation by qualified
   name and combines their results under an explicit, authored policy (e.g.
   "both must succeed," "either succeeding is sufficient," "run both in
   order, report the second's result"). This keeps each family's own
   `*Concrete` method distinctly named and uncontaminated — the collision
   and its resolution live entirely at the one point where two public names
   coincide, not smeared across the leaf's internals.

2. **Composition instead of inheritance.** If the two constraints are not
   actually meant to co-exist on one entity — if combining them would in
   truth produce a conflated, "mutated" type rather than a coherent one —
   the correct fix is to **not** inherit both families onto the same leaf at
   all. Instead, the leaf holds a reference (RID) to a separate entity of
   the other family, spawned via `addTag<OtherType>()`, and delegates to it
   externally through the ordinary entity-call surface. The ontology's
   answer to "these two things don't actually belong on the same object" is
   composition via addressable reference, not a forced merge.

Which of these two paths is correct is a judgment call for the leaf's
author, made explicit by the compiler error rather than silently defaulted
by the language.

---

## 6. Summary of the constraint model

| Relationship | Discovered when | Exclusivity source | Escape hatch |
|---|---|---|---|
| Lineage (`X → Y`) | Authored, up front | N/A — cumulative, not exclusive | N/A |
| Siblings (`Y`, `Z` under `X`) | Authored, up front | Same constraint, different specialization | None — genuinely exclusive |
| Orthogonal families (`Ephemeral_` + `ConnectionState_`) | Authored, at fold-in | N/A — independent axes | N/A — always combinable |
| Incidental name collision (`C`, `D`) | Lazily, at first attempted combination | Coincidental name/signature overlap | Explicit merge, or composition via `addTag` |

The overall effect is an ontology whose full constraint surface is not
entirely legible from any single interface file in isolation: two families
with no authored relationship can become mutually exclusive purely because
their independently-chosen vocabularies happened to overlap, and that fact
only becomes visible — and must be resolved — at the moment someone actually
attempts to combine them.
