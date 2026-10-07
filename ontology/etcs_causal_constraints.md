# The Causal family: constraints, their checks, and what is not promised

*The invariants `ontology/Causal.h`, `CausalBase.h` and `OrderVector.h` uphold,
each with the predicate or tester that says whether it still does. The
implementation behind each is free to change -- as optimal or as plain as a
purpose needs -- while its line here keeps passing. Companion to
`etcs_ontology_constraint_sets.md`, which says what a family IS; this says
what this one PROMISES.*

Checked by `loaders/OrderVectorTesterLoader.cc` (the rows alone),
`loaders/CausalTesterLoader.cc` (a tree of bodies) and
`loaders/SolidTesterLoader.cc` (planes, fields, solids). A constraint with no line
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

## 2. Same lines, same rows -- on every runtime

**Constraint.** Two runtimes replaying the same lines reach the same rows bit
for bit. No runtime fact (a RID's value, a map's walk order, a hash table's
bucket, a pointer) is an input to the physics or to the hash. What IS an
input, besides the lines, is one thing only: between twins -- members of one
container, one tag, one identity -- creation order, the last key, the same
one a persistence record is looked up by.

**How.** Row 0's identity is the identity TUPLE: the entity's identity hash
(`Entity::identityHash` -- the merkle over tags, relations, dispatch and
children; no RIDs, no values) mixed with its index among its twins
(`Entity::siblingIndex`, attach order). A crossing's uncertainty is derived
from the crossing's rows and span. Members are walked, and the hash
composed, in canonical order: tag first-attachment order, then identity
hash, then attach order between twins. `RIDList` enumerates in arrival
order, never the map's.

**What that means for a rebuilt scene.** The same lines in the same order:
the same hash. A distinguishable member attached in another place: the same
hash -- its place is by what it is. Twins attached in the other order: a
different hash, because the first twin is whichever came first, and that is
the one fact the lines carry about them.

**What that means for runtimes sharing one world.** The lines must be all
that steps it. A picture steps a scene by the wall clock (being looked at
is an interaction, §7), and the clock and each runtime's own eye are what
two runtimes do not share -- so a world replayed from one record is stepped
by its driver alone (RenderProvider's `Scene3D.SetDriven`), with the ticks
as lines of that record (KartProvider's battle).

**Check.** `CausalTester` §4 (the same lines twice; a distinguishable member
moved; twins swapped; the tuple on the rows). `HashTester` §10: arrival
order on every read. `KartTester` §6: three battles, one judging and two
following its record, the same battle to the bit.

## 3. Energy is conserved, exactly

**Constraint.** Over any sequence of interactions, the energy held by every
body in a tree plus what left at the open boundary (`EmittedOut`) equals what
`Impulse` put in, to the bit. A crossing is one number leaving one body and
landing in another: `Absorb(crossing)` adds exactly `crossing.energy`. A
field's work is the one other way in (§12): it is counted where the field is
stated (`FieldWork`), and the ledger is held less it.

**Check.** `CausalTester` §1 (2000 interactions over two containers, three
bodies, contacts included); `OrderVectorTester` §4b (a contact pair, before
and after, equal to the bit).

## 4. The arrow of time

**Constraint.** The ordered energy of a tree -- Σ|K| over its bodies -- never
rises except through work done from outside: `Impulse`, or a field's
`Accelerate` (§12). Drag, emission, absorption and contact each
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

## 7. The lazy commit is exact, and the order is commit, step, members, contacts, fit

**Constraint.** Emission over an interval is `h·(1 − e^(−k·dt))`, so one
commit over T equals N commits across T while nothing else touches the heat
-- which is why the commit is the first thing an interaction does, before the
step adds the next interval's heat. Then the members (each the same way),
then the contacts among them, from the rows the steps left, then what each
member is in (§11), from the rows the contacts left.

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

**How the pairs are found is free; which pairs, and in what order, is not.**
A pair's crossings change energy the later pairs read, so the contacts are
the n² loop's pairs in its (i, j) order. A large container finds them with a
kd-tree over the members' positions (`CausalBase::Broadphase`, nanoflann in
`libs/`): a superset, padded past what doubles and the gate's floored squares
can differ by, which the exact gate then decides. The gate itself compares
without wrapping (`OrderVector::MayInteractWith`: each square floored as a
Fixed product, summed wide), so a pair far apart never passes.

**Check.** `CausalTester` §5 (head-on, equal masses: the mover stops and the
other carries all of it, where the reaches met; the emitter's clock ticked);
§9 (crowds of 400 and 1,000 stepped through the kd-tree and pair by pair:
the same rows to the bit; contacts moved bodies nothing pushed);
`OrderVectorTester` §4 (two reaches 65,536 apart do not touch; 200,000 pairs
where nothing wraps answer as the Fixed squares did);
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

## 10. The rows are what is kept, and they are on the surface

**Constraint.** A driver's spans are in the script. An observer's spans --
the wall clock, capped, made Fixed -- are the whole of what the clock
contributed, and the rows after them are their fixed point: the same spans
into a fresh tree land on the same rows. What is kept is the rows, as the
value behind the `Causal` tag on the entity's own surface (`Entity::
bindValue`; `CausalBase::packState`/`unpackState`): every word `CausalHash`
reads and every reading the family offers, so a tree rebuilt by its lines
and handed the captured values is the live tree -- same hash, same clock,
and it goes on the same way. The family keeps no record of its own; the one
store (Persistence) keeps the surface, and the one state hash
(`Entity::getHash`: the identity half and every value under the node) is
what says a resumed scene is the saved one.

**Check.** `CausalTester` §6 (600 observed interactions with random spans
into two trees: same hash; the values captured off one and restored onto a
third built by the same lines: same hash, same clock, same next 100 ticks;
a value of another version refused). `ProvenanceTester` §7b for the surface
itself.

## 11. What holds a thing is what it fits in

**Constraint.** Every Causal thing is an environment for what fits in it: the
universe holds the earth, which holds the person, which holds the pen in
their pocket. A member is in its container while its reach lies inside the
space the container provides (`Space`, a radius about its position, set by
`SetSpace` through the funnel; zero is solid and holds nothing). After the
contacts, every interaction, from the rows alone: a member inside a
sibling's space moves into the smallest such sibling; one outside its
container's space moves up to the container's container; the open boundary
keeps what fits nowhere. Decided before anything moves, applied in canonical
order, and a member something moves into stays put that time. A move keeps
where the thing is in the world exactly (frames are translations: a member's
position is relative to its container's) and touches no energy, so §3 holds
across it.

**What a move is.** The entity changes parents (`Entity::moveTo`): the
funnel's own event (a TagModify carrying the move, ordered against every tag
operation on the child and both parents), recorded as its own action,
`<to>.Contain(@<it>)`, so a replay keeps the last move and a step's moves are
never credited to the line that ran the step. Its lifetime goes with it: a
Causal child's bytes live in its module's root arena, and only its lifetime
token -- its destructor record and its arena's -- sits in its parent's
chain (`MemoryArena::adoptToken`, then `moveToken` on every move); deleting
where it came from leaves it, deleting where it went takes it, and a parent
deleted without its children hands them up with their tokens.

**The environment answers.** `Environment` (its container), `Basis` (where
its frame sits in the topmost one's), `Near` (members within reach of a point
of the frame), `Adjacent` (what shares its environment and touches its
reach).

**Check.** `CausalTester` §10 (a solid thing's member moves up to the same
place; the pen leaves the pocket into the person, then the world; the ball
rolls into a hollow box's space and out the far side; the ledger every tick;
`Contain` keeps the world position to the bit and refuses a cycle; the same
lines twice, the same moves and hash; deleting the old parent leaves it, the
new one takes it). `ProvenanceTester` §9 for the move itself (its own
recorded action; the last move kept; a parent deleted without its children
hands a movable one up, its token with it). A persistence round trip of a
scene with a move comes back as it was.

## 12. A space has parameters, and what is inside is subject to them

**Constraint.** A container states what its space is like, and its members
are subject to it. The radius of §11 is one parameter (what fits); the field
is another (`SetGravity`, the value behind "gravity"): every Causal member
feels the field of the nearest container that states one. Cumulative down the
tree by default -- a container that states nothing passes on the field it is
in -- and only by default: a container may state its own, zero included,
whatever its parent's is (`InheritGravity` takes the statement back), and
may provide more room than it takes up in its parent. A field changes a
member's velocity by `g·dt` before its step (`OrderVector::Accelerate`), and
E moves by exactly the kinetic change; the work, signed, is counted on the
container that states the field, so §3 holds with it. An anchored body
(`SetAnchored`) is held where it is: the field moves it not, and motion
handed to it becomes heat. A container that is not anchored is a thing and
falls in its own container's field carrying its members.

**Check.** `SolidTester` §2 (a room that states nothing passes the world's
field in; a pocket stating zero holds its member still; taken back, it
inherits; a body thrown up and falling back; the ledger each time).

## 13. Solids meet by their shape, through planes

**Constraint.** A plane (`ontology/Plane.h`) is a unit normal and an offset in
the rows' own integers: a side for a point or a reach, a split for a set
(`Bisect`), a crossing for a ray, the faces of a solid. A body made solid
(`SetSolid(restitution, friction)`, the value behind "solid"; `SetShape`:
sphere, box, half-space) meets another solid by shape instead of by
transmission (§8), and meets nothing else -- a body that is not solid passes
through it, so the two contact models never meet in one pair: a sphere against a sphere, a box (six planes about its
centre, row 3 facing them) or a half-space (one plane through its centre,
facing row 3's up). Each pair, after the pass's transmissions, from the pairs
the gate found on the rows the pass started with -- so the kd path and the
pair-by-pair path meet the same pairs in the same order (a half-space has no
reach and meets every solid):

- **apart**: the sphere is pushed out along the normal by how deep it is
  (both, by inverse mass, when neither is anchored) -- positions move here
  and only here;
- **bounce**: the closing motion along the normal is turned back times the
  restitution -- of the approach less one step of the field, since the push
  out undoes that step -- and a bounce smaller than two steps of the field is
  a landing;
- **rub**: the motion across the normal loses friction times the load (the
  bounce's impulse, plus what the field presses a resting body in with),
  never more than stops it.

Motion lost is heat in the body that lost it (`SetVelocity` holds E); motion
one free body hands another leaves the giver's heat and arrives in the taker's
first, so §3 holds and §4's arrow holds. A body left closing slower than a
landing with the field pressing it in rests on the other until the next pass
(`SupportUnder`): the field's part into that surface is not applied, so a
resting body is still and stays so, and a body on a slope feels only the part
along it.

A body brought to rest has no kinetic energy, so the interaction that rests
it ends with nothing under the container `Moving()`: that is what an observer
asks before looking again (Scene3D draws a ball in flight on a still green,
and stops drawing when it settles).

**Check.** `SolidTester` §1 (planes), §3 (a drop: each apex lower, then at
rest on the surface and held there), §4 (a roll stops near v²/2μg; without
friction it does not; `Moving` while it rolls, not after), §5 (a slope), §6 (two free balls exchange velocities),
§7 (a half-space far from its centre), §8 (a crowd of 60: the same rows pair
by pair, through the kd-tree, and twice; the ledger), §9 (a ball rolled at a
gap in an anchored green drops into a cup -- a container with its own space
and floor -- is taken in by fit and rests on the cup's floor).

---

## Not promised (yet)

- **Moving frames.** A member's position is relative to its container's, so
  it goes where the container goes; its motion is not. A move restates the
  position, never the velocity -- doing so would change kinetic energy, and
  §3 is exact.
- **Moves between modules.** A thing moves only under parents of its own
  module (its bytes and token live in that module's arenas).

- **Restitution and separation between bodies that are not solid.** A
  contact between bodies that are not both solid is transmission: the energy
  along the line goes to the other body entirely, and overlapping members
  stay overlapping. Both are §13's, for solids.
- **A box against a box.** Solids meet when one of them is a sphere; two
  boxes (or a box and a half-space) pass through each other.
- **Free fall as a frame.** A container that falls carries its members, and
  they feel the field in its frame as well -- twice the fall. A place is
  anchored; the equivalence of a falling frame is not modelled.
- **Rest across a resume.** The surface a body rests on is not kept; the first
  contact after a resume finds it again (one step of the field, landed).
- **Angular rate.** Row 3 is a facing; the share of E that is rotational is
  not carried, so contacts transfer no spin and `Reduce` does not sum orbital
  motion.
- **Replay between captures.** The observed spans are not kept; a resumed
  scene continues from its last captured rows, it does not re-run the
  frames between that capture and the close.
- **Reach of a container on the driver path.** A member's reach is set where
  it is made (Scene3D: at `Create`); a container's reach over its members is
  recomputed on the observed path (`coverRows`) and not by the driver.
- **Mass in transmission.** Transmission hands over energy, not momentum;
  unequal masses exchange energy as if equal. Honest, conservative, and not
  Newtonian in the second body's speed. A solid contact (§13) is Newtonian:
  impulses by inverse mass.
