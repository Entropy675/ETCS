# The Causal family: constraints, their checks, and what is not promised

*The invariants `ontology/Causal.h`, `CausalBase.h` and `OrderVector.h` uphold,
each with the predicate or tester that says whether it still does. The
implementation behind each is free to change -- as optimal or as plain as a
purpose needs -- while its line here keeps passing. Companion to
`etcs_ontology_constraint_sets.md`, which says what a family IS; this says
what this one PROMISES.*

Checked by `loaders/OrderVectorTesterLoader.cc` (the rows alone) and
`loaders/CausalTesterLoader.cc` (a tree of bodies). A constraint with no line
in either is not a constraint yet; it is a sentence.

---

## 1. The rows are the state; a picture is derived

**Constraint.** Everything causal about an entity is in its `OrderVector`
(four rows of Q32.32 integers: where, order, reach, facing) and its clock.
Nothing that reaches the rows is a float: the one float, `ToMatrix4`, is a
projection out of them, and a runtime that makes no picture runs the same
rows.

**Check.** `OrderVectorTester` §1 holds Fixed's arithmetic and functions to
the bit across both spellings (`__int128`, halves); §5 runs one sequence
twice to the same hash. `scene3d_run.etcs` prints a hash a browser must
repeat (the reference is in `claude/order-vector-device.md`).

## 2. Same lines, same rows -- on every runtime, in any creation order

**Constraint.** Two runtimes replaying the same lines reach the same rows bit
for bit. The same scene built with its members attached in another order is
the same scene: same `CausalHash`, same rows per member, same draws. No
creation-order value (a RID, a map's walk order, a hash table's bucket) is
an input to the physics or to the hash.

**How.** Row 0's identity is the entity's state hash (`core/Entity.h`,
no RIDs); a crossing's uncertainty is derived from the crossing's rows and
span alone; members are walked in canonical order (tag first-attachment
order, then state hash, then -- only between members of one state, which the
physics cannot tell apart -- attach order); `CausalHash` composes the members
as a sorted multiset. `RIDList` enumerates in arrival order, never the map's.

**Check.** `CausalTester` §4: the same world built forward and reversed, run
1500 ticks, one hash; the identities on the rows agree across the builds.
`HashTester` §10: arrival order on every read.

## 3. Energy is conserved, exactly

**Constraint.** Over any sequence of interactions, the energy held by every
body in a tree plus what left at the open boundary (`EmittedOut`) equals what
`Impulse` put in, to the bit. A crossing is one number leaving one body and
landing in another: `Absorb(crossing)` adds exactly `crossing.energy`.

**Check.** `CausalTester` §1 (2000 interactions over two containers, three
bodies, contacts included); `OrderVectorTester` §4b (a contact pair, before
and after, equal to the bit).

## 4. The arrow of time

**Constraint.** The ordered energy of a tree -- Σ|K| over its bodies -- never
rises except through `Impulse`. Drag, emission, absorption and contact each
either hold it or lower it. Per body: `Dissipate` lowers |K|, `Emit` and
`Advance` hold it, a contact lowers the emitter's by at least what it can
raise the absorber's.

**Check.** `CausalTester` §1; `OrderVectorTester` §4b fuzz (20,000 random
operations on a pair, measured with `KineticEnergy`, which is taken as the
length of K = O·E so its error is a few last bits whatever |O| is).

## 5. The rows hold

**Constraint.** For every body after every interaction: E ≥ 0, reach ≥ 0,
|O| ≤ 1, row 3 a unit quaternion -- each within `OrderVector::kSlackRaw`
(2⁻¹⁶), the slack a truncating normalisation needs. `OrderVector::Holds()`.

**Check.** `OrderVectorTester` §4b fuzz; `CausalTester` §1 and §7.

## 6. Emission is the clock, and the last quantum leaves whole

**Constraint.** `CausalTicks` counts crossings this body made (entropy
commits and contact crossings), not visits. A body with no heat makes no
entropy crossing and its clock stands still. A cooling body reaches exactly
zero heat: once the share of what remains truncates to nothing, the whole of
it goes (`CommitShare`).

**Check.** `CausalTester` §8; `OrderVectorTester` §4b (last quantum).

## 7. The lazy commit is exact, and the order is commit, step, members, contacts

**Constraint.** Emission over an interval is `h·(1 − e^(−k·dt))`, so one
commit over T equals N commits across T while nothing else touches the heat
-- which is why the commit is the first thing an interaction does, before the
step adds the next interval's heat. Then the members (each the same way),
then the contacts among them, from the rows the steps left.

**Check.** `OrderVectorTester` §2 (emission over T equals emission in parts);
the order is in `CausalBase::InteractUnder` and nowhere else.

## 8. One relation, adjacency; one event, a crossing

**Constraint.** A crossing passes only between adjacent entities, and there
are two adjacencies: standing (a member and its Causal parent; a member's
entropy crosses up, always) and discovered (two members of one container
whose reaches touch, `GapTo ≤ 0`, decided from the rows alone, in the
container's frame, every interaction). A container is an entity whose
adjacency set includes its members, nothing more; at the root the missing
parent is the open boundary. A contact hands over the part of the ordered
energy headed along the line between the two (`CrossToward`: KE·cos²θ), and
both crossings are taken before either lands, so the exchange is simultaneous
and does not depend on which member is walked first.

**Check.** `CausalTester` §5 (head-on, equal masses: the mover stops and the
other carries all of it, where the reaches met; the emitter's clock ticked);
`OrderVectorTester` §4b (45°: half crosses, half stays, perpendicular; a body
moving away hands over nothing). §4's forward/reversed build covers the walk
order.

## 9. One history

**Constraint.** Every write to any rows in a tree is in one total order: a
driver's `Run` and an observer's `Interact()` take turns on the tree, never
interleaving inside a step. A read of the rows is of one state only under
`TreeMutex()`; outside it a read may straddle a step, and the family says so
(`Order4`).

**Check.** `CausalTester` §7 (a reader under the lock, a driver on another
thread, 10,000 ticks: every read one state, every row holding).

## 10. Every input that reaches the rows is recorded

**Constraint.** A driver's spans are in the script. An observer's spans --
the wall clock, capped, made Fixed -- are the whole of what the clock
contributed, and they go on the observed root's tape (`InteractObserved`,
`ObservedTape`); a fresh tree replaying the tape lands on the same rows. The
tape is bounded and says when it stopped (`ObservedTapeFull`) rather than
wrapping.

**Check.** `CausalTester` §6 (600 observed interactions with random spans,
replayed onto a fresh tree: same hash, same clock).

---

## Not promised (yet)

- **Restitution.** A contact is transmission: the energy along the line goes
  to the other body entirely. A coefficient of restitution is a parameter on
  that one operation, not written.
- **Separation.** Two overlapping members at rest stay overlapping; nothing
  pushes them apart. Penetration is not resolved.
- **Angular rate.** Row 3 is a facing; the share of E that is rotational is
  not carried, so contacts transfer no spin and `Reduce` does not sum orbital
  motion.
- **Broadphase.** Contacts are every pair of members, n² per interaction. The
  gate (`GapTo`) is the constraint; a cheaper way to ask it is an
  implementation choice.
- **Reach of a container on the driver path.** A member's reach is set where
  it is made (Scene3D: at `Create`); a container's reach over its members is
  recomputed on the observed path (`coverRows`) and not by the driver.
- **Mass in contact.** Transmission hands over energy, not momentum; unequal
  masses exchange energy as if equal. Honest, conservative, and not Newtonian
  in the second body's speed.
