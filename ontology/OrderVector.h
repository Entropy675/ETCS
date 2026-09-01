#ifndef ONTOLOGY_ORDERVECTOR_H__
#define ONTOLOGY_ORDERVECTOR_H__


#include "../core_defs.h"
#include <cmath>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// OrderVector — two linearly related rows of four, and the reserved base of a
// 4x4 whose rows are a distinguishable unit of space.
//
//     row 0   (x,  y,  z,  RID)      where it is, and which thing it is
//     row 1   (Ox, Oy, Oz, E)        where its energy is going, and how much
//
// THE TWO ROWS ARE LINEARLY RELATED IN THEIR FIRST THREE COMPONENTS: row 1 is
// the rate row of row 0, so integrating it is what moves the point, and the
// relation between them is the only thing that makes this a state rather than
// two facts. The FOURTH components are what the relation conserves rather than
// what it mixes -- identity on the position row, total energy on the order row
// -- which is why they are the two slots a later constraint has to hold fixed
// across every row of the matrix.
//
// THE ORDER VECTOR IS A DIRECTION AND A FRACTION AT THE SAME TIME. O is not a
// velocity and not a unit vector: |O| lies in [0,1] and is the SHARE of E that
// is kinetic along O/|O|. Everything else follows from that one reading:
//
//     kinetic vector K = O * E          -- direction of travel, magnitude of
//                                          the energy carried along it
//     kinetic energy   |K| = |O| * E
//     heat             E - |K| = (1 - |O|) * E
//     invariant        |O| <= 1, equivalently |K| <= E
//
// so a point with |O| = 1 has all of its energy in motion, |O| = 0 has all of
// it as heat and does not move, and the two are one number apart rather than
// two quantities that have to be kept in step.
//
// WHY THAT IS WORTH THE INDIRECTION over storing a velocity. Drag stops being
// an erasure and becomes a TRANSFER: damping scales |K| down and leaves E
// alone, so the energy that left the motion is exactly the energy that arrived
// as heat, and nothing had to be subtracted from anywhere. A velocity vector
// times 0.98 destroys energy silently every tick and has nowhere to put it.
// Here the accounting is the representation.
//
// THE ARROW OF TIME IS ALSO STRUCTURAL, and it is the sharper of the two
// invariants: |K| NEVER RISES EXCEPT THROUGH Impulse. Dissipate lowers it,
// Emit holds it, Advance holds it, and Impulse -- work done on the point from
// outside -- is the only operation in the file that adds to it. So ordered
// energy decays into unordered energy and never spontaneously the other way,
// which is the second law stated as a property of the operation set rather
// than as a rule someone has to remember.
//
// Emit RAISES |O| while holding |K|, and that is not a counterexample: heat
// leaving shrinks the total, so the surviving motion is a larger share of a
// smaller number. The law is about the quantity, not the fraction. A point
// that has shed all its heat has |O| = 1 and has still never gained a joule
// of order.
//
// WHICH MAKES EMISSION THE CLOCK. Each commit of entropy is one tick of this
// point's own time, and the direction is inverted from the usual arrangement:
// time is not a thing imposed on the entity from outside that it is carried
// along by, it is a thing the entity EMITS. An entity shedding entropy quickly
// is running fast; one at rest with no heat left emits nothing, its counter
// stops, and that is correct -- nothing about it distinguishes one moment from
// the next, so it has no local time to keep. A wall clock is only ever used
// here to work out how much is owed at an interaction; what an entity's own
// history is measured in is how many times it has paid.
//
// A PRNG DRAW HAPPENS INSIDE AN EMISSION, IT IS NOT INDEXED BY ONE. This is
// the distinction the whole reserved design turns on, and getting it backwards
// is easy: a draw is not a separate event sequenced beside the tick and keyed
// off its number. The uncertainty of a boundary crossing IS what the emission
// carries, so a draw READS A PROPERTY OF THE EMISSION -- it occurs within that
// event, in the same lazy step, not after it.
//
// Which is why the step is an AXIS of time rather than a count of it. Where a
// guard captures and re-emits a generator, the captured event is nested in the
// emission, so everything settled by one commit is SIMULTANEOUS by
// construction: order exists between emissions and nowhere inside one. That is
// the same granularity argument the depth buffer makes about space
// (Drawable3D.h) -- an emission is the finest grain at which "before" is still
// a total order, and asking for order below it is asking a question the
// structure does not have an answer to.
//
// It is also what makes the laziness legitimate rather than an optimisation:
// deferring a commit cannot reorder anything, because nothing inside the
// deferred interval was ordered against anything else to begin with. And it is
// what makes a substituted generator deterministic without threading a seed
// anywhere -- replaying the same emission reproduces the same uncertainty
// because the uncertainty is derived from the emission's own content, and two
// clocks that would have to be kept in step never exist. Keeping them in step
// is precisely what cannot be done from inside a sandbox.
//
// AN EMISSION IS ITSELF AN OrderVector, which is the reason this file needs no
// second struct for one. What crosses a boundary is a quantity of energy AT a
// place, FROM an identity -- which is rows 0 and 1 exactly, and everything a
// reader wants is then derived by the same three functions as for any other
// point rather than restated as fields. It comes out with |O| = 0: emitted
// entropy is unordered by definition, so all of its energy is heat, and the
// zero is a statement rather than a default. An emission that ever carried a
// direction would be radiation pressure, and the representation for it already
// exists -- it is a non-zero O on the thing that crossed.
//
// So a transfer is one vector moving: EmitEvent hands it out, Absorb takes it
// in, and the receiving point gains the heat and any momentum it came with,
// through the operations that were already here. There is no separate ledger
// type that could disagree with the state it describes.
//
// Reserved, not implemented: the derivation is below, the count is on the
// bodies (Scene3D::CausalTicks), and the guard substitution is not written. The
// reading is fixed now because it is the part that decides whether the two
// mechanisms can ever be one thing.
//
// THE INVARIANT IS STRUCTURAL, not checked. Impulse adds j joules along a unit
// direction: |K| grows by AT MOST j (triangle inequality) while E grows by
// exactly j, so |O| <= 1 survives every impulse, including one that fights the
// current direction. Dissipate scales |K| and leaves E, so it can only lower
// |O|. There is no sequence of the operations below that can leave a point
// with more kinetic energy than it has energy, which is why nothing here has
// to validate anything.
//
// ---------------------------------------------------------------------------
// ROWS 2 AND 3: where it turns, and how far round it is.
//
//     row 2   (Fx, Fy, Fz, -)        rotational pivot
//     row 3   (Sx, Sy, Sz, theta)    axis and angle
//
// ROW 2 MAKES ROTATION A RELATION TO A POINT rather than a property of a body.
// The pivot is stated in this vector's OWN frame -- relative to row 0, the same
// parent-relative rule the entity tree runs on -- so a sub-unit turning about
// its parent's pivot holds no copy of it, and moving the parent moves what
// everything under it turns about, for free. F of zero is the ordinary case: a
// thing that turns about itself.
//
// ROW 3 IS AN ORIENTATION, axis-angle, and the axis being a VECTOR rather than
// three Euler angles is the whole reason it is worth the row. Euler angles have
// an order, gimbal-lock at the poles, and no composition that is not a special
// case; an axis and an angle compose by one multiplication and degenerate
// nowhere. That is not a numerical preference -- yaw and pitch as two floats is
// exactly what was wrong with the look control this replaced.
//
// A ZERO AXIS MEANS UNROTATED, which is why the identity needs no sentinel and
// why theta is meaningless without S beside it. Angular RATE -- the share of E
// that is rotational, which would be row 3's answer to what O is for row 1 --
// is deliberately not here yet: the linear constraints are enough to carry
// causality for now, and adding a rate before there is anything to spin would
// be fixing the shape of a thing nothing has yet asked for.
//
// THE FOURTH SLOT OF ROW 2 IS THE RADIUS, and it is what distinguishes an
// aggregate from a leaf without any second type being involved. r == 0 is a
// point -- the only kind of thing that is not an aggregate of anything -- and
// r > 0 says this vector summarises a set, with every other quantity on it
// being a reduction over that set rather than an independent value that could
// disagree with it.
//
// THE PRESENCE OF THE CONSTRAINT IS THE DIFFERENCE, not the presence of a
// field. Splitting the aggregate off into its own struct was the first thing
// tried here and it is wrong: the rows are a property of what an OrderVector
// IS, and a parent and a child that could not be handed to the same function
// would make the reduction impossible to write once. One type, one set of
// operations, and a leaf is simply the case where the radius is zero -- which
// is also the case where Reduce over a single member returns it unchanged.
//
// ROWS ARE STRUCTURAL EVEN WHERE INERT. A flat node carries rows 2 and 3 and
// never turns; a still node carries row 1 and never moves. That is not waste,
// it is what makes one vector describe a thing at any level of the composition
// and at any dimensionality -- the alternative is a family of nearly-identical
// records and a conversion between each pair of them.
// ---------------------------------------------------------------------------

struct OrderVector
{
    // ── row 0: the point ────────────────────────────────────────────────
    //
    // RID in the fourth slot rather than beside the struct: identity is a
    // coordinate here. Two units at the same position are still two units,
    // and a reduction over leaves has to be able to say which leaves.
    float     x   = 0.0f;
    float     y   = 0.0f;
    float     z   = 0.0f;
    ETCS::RID rid = 0;

    // ── row 1: the order ────────────────────────────────────────────────
    float ox     = 0.0f;
    float oy     = 0.0f;
    float oz     = 0.0f;
    float energy = 0.0f;

    // ── row 2: the pivot, and how far this vector reaches ───────────────
    //
    // F is in this vector's own frame, so it moves with row 0 and needs no
    // recomputation when its container moves. Zero is the ordinary case: a
    // thing that turns about itself.
    //
    // The radius is the fourth component and the aggregate/leaf constraint:
    // zero means a point, positive means this vector summarises a set that
    // reaches that far from row 0.
    float fx     = 0.0f;
    float fy     = 0.0f;
    float fz     = 0.0f;
    float radius = 0.0f;

    // ── row 3: the angle ────────────────────────────────────────────────
    //
    // S is the axis, unit or zero; theta is the angle about it, in radians.
    // Zero axis is the identity -- an unrotated thing, with no sentinel.
    float sx    = 0.0f;
    float sy    = 0.0f;
    float sz    = 0.0f;
    float theta = 0.0f;

    // ── meta: real, and not yet rows ────────────────────────────────────
    //
    // Both are properties of the causal step this vector represents, and both
    // are here rather than in a companion struct because a companion struct is
    // a second thing that can disagree with the first. They are called meta
    // only in the sense that the 4x4 has not been cut yet: `interval` is the
    // span the step settles, which is what row 2's pivot and radius will be
    // stated over once composition is real, and `uncertainty` is what a draw
    // nested inside the step reads, which is angular the moment row 3's spinor
    // exists. Solidifying them means moving them into those rows, not
    // inventing them then.
    //
    // On a BODY they describe its most recent crossing and are current state;
    // on an EMISSION they describe that emission and are fixed. Same fields,
    // and the difference is only which of the two the vector is.
    float    interval    = 0.0f;
    uint64_t uncertainty = 0;

    // ── readings of the relation ────────────────────────────────────────

    float KineticFraction() const { return std::sqrt(ox * ox + oy * oy + oz * oz); }
    float KineticEnergy()   const { return KineticFraction() * energy; }
    float Heat()            const { return energy - KineticEnergy(); }

    // The direction of travel, or the zero vector when there is none -- which
    // is the honest answer for a point whose energy is all heat, rather than
    // an arbitrary axis that would make it drift the moment it was accelerated.
    void Direction(float& dx, float& dy, float& dz) const
    {
        const float m = KineticFraction();
        if (!(m > 0.0f)) { dx = dy = dz = 0.0f; return; }
        dx = ox / m; dy = oy / m; dz = oz / m;
    }

    /*
 * Speed from kinetic energy: |K| = 1/2 m v^2, so v = sqrt(2|K|/m).
 *
 * Mass is a parameter rather than a fifth field because it belongs to the
 * body, not to the point -- a leaf and the sphere aggregating it have the
 * same energy accounting and different masses, and storing it here would
 * make the reduction over leaves have to reconcile two of them.
 */
    void Velocity(float mass, float& vx, float& vy, float& vz) const
    {
        vx = vy = vz = 0.0f;
        if (!(mass > 0.0f)) return;
        const float ke = KineticEnergy();
        if (!(ke > 0.0f)) return;
        float dx, dy, dz;
        Direction(dx, dy, dz);
        const float v = std::sqrt(2.0f * ke / mass);
        vx = dx * v; vy = dy * v; vz = dz * v;
    }

    // ── the operations ──────────────────────────────────────────────────

    /*
 * Add `joules` of kinetic energy along a UNIT direction -- an engine, a
 * thruster, a key being held.
 *
 * Energy enters the point: E grows by exactly j, K grows by at most j. That
 * asymmetry is the whole reason the invariant needs no check, and it is also
 * physically the right shape: pushing against your own motion costs the same
 * energy as pushing with it and buys you less speed, and here the difference
 * lands in heat by construction rather than by a special case.
 */
    void Impulse(float dx, float dy, float dz, float joules)
    {
        if (!(joules > 0.0f)) return;
        const float m = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!(m > 0.0f)) return;

        float kx = ox * energy, ky = oy * energy, kz = oz * energy;
        kx += (dx / m) * joules;
        ky += (dy / m) * joules;
        kz += (dz / m) * joules;
        energy += joules;
        setKinetic(kx, ky, kz);
    }

    /*
 * Scale the kinetic share by `factor` and leave E alone: drag, friction,
 * inelastic contact -- every process that takes energy out of motion without
 * taking it out of the point.
 *
 * The difference becomes heat because heat is DEFINED as the remainder
 * (E - |K|), so there is nothing to add anywhere. That is the representation
 * doing the accounting, and it is why this is one multiply rather than a
 * subtract-here-add-there pair that can drift.
 */
    void Dissipate(float factor)
    {
        if (factor < 0.0f) factor = 0.0f;
        if (factor >= 1.0f) return;
        ox *= factor; oy *= factor; oz *= factor;
    }

    /*
 * ENTROPY EMISSION -- how much heat leaves this point over dt, at this
 * emissivity, into whatever contains it.
 *
 * Dissipate turns motion into heat and stops there, which leaves every point
 * accumulating heat forever: an accounting that conserves energy and models
 * nothing, because a real body at rest in a colder environment gives its heat
 * up. This is the second half, and the two together are the whole of what
 * "friction" is -- kinetic to heat, heat to the surroundings.
 *
 * EXPONENTIAL IN THE HEAT CURRENTLY HELD, which is what makes committing it
 * LAZILY exact rather than approximate. Emitting h*(1-exp(-k*T)) once for an
 * interval T is identical to emitting N times across the same T, because the
 * fraction is the same per unit time whatever the sampling -- so a point that
 * has not interacted with anything for a while owes exactly one calculation,
 * not one per tick it slept through. A linear rate would not have this
 * property, and a point that idled would emit an amount that depended on how
 * often nobody looked at it.
 *
 * The identity holds only while NOTHING ELSE touches the heat during the
 * interval, which is a real constraint and the reason the commit belongs at
 * the same instant as the drag rather than on a clock of its own: if the two
 * are interleaved at the same points, every interval is clean by
 * construction.
 */
    float EmissionOver(float dt, float emissivity) const
    {
        if (!(dt > 0.0f) || !(emissivity > 0.0f)) return 0.0f;
        const float h = Heat();
        if (!(h > 0.0f)) return 0.0f;
        return h * (1.0f - std::exp(-emissivity * dt));
    }

    /*
 * Take `joules` of HEAT out of the point, and return what actually left --
 * capped at the heat there is, since a point cannot emit motion.
 *
 * E falls and K is held: the kinetic vector is recomputed against the new
 * total so that |K| is unchanged and |O| RISES. That is not a side effect to
 * be tolerated, it is the correct reading -- a point that has shed its heat
 * carries a larger fraction of its remaining energy as motion, and a body
 * whose heat is all gone has |O| = 1.
 *
 * Emit and Absorb are the same operation with opposite sign, which is why
 * they are two lines each: a transfer between two points is one Emit and one
 * Absorb of the identical number, and there is nowhere for energy to be
 * created or lost between them.
 */
    float Emit(float joules)
    {
        if (!(joules > 0.0f)) return 0.0f;
        const float q = (joules > Heat()) ? Heat() : joules;
        if (!(q > 0.0f)) return 0.0f;
        const float kx = ox * energy, ky = oy * energy, kz = oz * energy;
        energy -= q;
        setKinetic(kx, ky, kz);
        return q;
    }

    /*
 * Emit, as the EVENT rather than the number -- the form a guard can nest
 * inside.
 *
 * Same transfer as Emit above, and it returns the scope: the joules that
 * crossed, the span they settle, whose boundary it was, and the uncertainty
 * carried by that crossing. Nothing is stored: the emission is handed to the
 * caller, who is the only one that knows what else belongs inside it.
 *
 * The derivation mixes the three facts that make this crossing THIS crossing
 * -- identity, quantity, span. It is a splitmix-style finaliser over their
 * bits, which is a hash and not a generator: there is no state to advance,
 * because a stream position would be the second clock this whole reading
 * exists to avoid. Quantity and span are taken by their bit patterns rather
 * than rounded, so an emission that differs at all differs everywhere.
 *
 * A zero emission has zero uncertainty and is not an event: nothing crossed,
 * so there is nothing for anything to be nested in, and a draw at that
 * moment has no boundary to read.
 */
    OrderVector EmitEvent(float joules, float span)
    {
        // Row 0 of what leaves is where it left from and whose boundary it
        // crossed -- the emitter's own row 0, unchanged. An emission has a
        // place and an identity for the same reason any other point does.
        OrderVector e;
        e.x = x; e.y = y; e.z = z;
        e.rid      = rid;
        e.interval = span;

        // Row 1: all of it, and none of it ordered. |O| = 0 is the statement
        // that what crossed was entropy -- see this file's header. The energy
        // is whatever Emit could actually take, which is capped at the heat
        // there was.
        e.energy = Emit(joules);
        e.ox = e.oy = e.oz = 0.0f;
        if (!(e.energy > 0.0f)) return e;   // nothing crossed: not an event

        e.uncertainty = derive_uncertainty(e);
        return e;
    }

    // The receiving end: heat in, motion untouched. An environment absorbing
    // its contents' entropy gets warmer and does not start moving, which is
    // exactly what holding K while raising E says.
    void Absorb(float joules)
    {
        if (!(joules > 0.0f)) return;
        const float kx = ox * energy, ky = oy * energy, kz = oz * energy;
        energy += joules;
        setKinetic(kx, ky, kz);
    }

    /*
 * Absorb a whole crossing -- the other half of EmitEvent, and the form a
 * transfer actually takes: one vector leaves one point and arrives at
 * another, with nothing converted on the way.
 *
 * Its heat lands as heat and its ordered part lands as an impulse, which is
 * the correct reading and not two cases: energy that arrived with a
 * direction pushes. Today every emission has |O| = 0 so only the first line
 * ever fires -- but the second is what radiation pressure would already be,
 * with no new operation, which is the point of the emission being an
 * OrderVector rather than a number.
 *
 * Absorbing does NOT copy the crossing's meta. interval and uncertainty
 * belong to the step that PRODUCED it; the receiver's own step is a
 * different one, and taking them would be claiming somebody else's clock.
 */
    void Absorb(const OrderVector& q)
    {
        const float heat = q.Heat();
        if (heat > 0.0f) Absorb(heat);

        const float ke = q.KineticEnergy();
        if (ke > 0.0f)
        {
            float dx, dy, dz;
            q.Direction(dx, dy, dz);
            Impulse(dx, dy, dz, ke);
        }
    }

    // Move the point by one step of its own rate row. The linear relation
    // between the two rows, applied -- the only place row 1 touches row 0.
    void Advance(float dt, float mass)
    {
        if (!(dt > 0.0f)) return;
        float vx, vy, vz;
        Velocity(mass, vx, vy, vz);
        x += vx * dt; y += vy * dt; z += vz * dt;
    }

    // Put the point down somewhere, with no energy implication. A teleport is
    // a legitimate thing for a script to ask for and is NOT a motion: it does
    // not change what the point is carrying, only where it is carrying it.
    void PlaceAt(float px, float py, float pz) { x = px; y = py; z = pz; }

    // All energy to heat, motion to rest. What a collision with something
    // immovable does, and what a released control eventually reaches.
    void Rest() { ox = oy = oz = 0.0f; }

    // ── row 2 / row 3 operations ────────────────────────────────────────

    void SetPivot(float px, float py, float pz) { fx = px; fy = py; fz = pz; }
    bool Oriented() const { return sx != 0.0f || sy != 0.0f || sz != 0.0f; }

    // Set the orientation outright. The axis is normalised on the way in, so
    // callers may hand over any non-zero vector and the stored row is always
    // in the form every reader below assumes.
    void Orient(float ax, float ay, float az, float angle)
    {
        const float m = std::sqrt(ax * ax + ay * ay + az * az);
        if (!(m > 0.0f)) { sx = sy = sz = 0.0f; theta = 0.0f; return; }
        sx = ax / m; sy = ay / m; sz = az / m;
        theta = angle;
    }

    /*
 * Compose a delta rotation onto this one, through quaternions.
 *
 * The delta is applied on the LEFT -- R_new = R_delta * R_current -- which is
 * what makes a mouse movement mean "turn from where you are now" rather than
 * "turn in your own local frame", and it is the difference between a look
 * control that behaves and one that corkscrews as soon as it is pitched.
 *
 * Quaternions in the middle and axis-angle at the edges: the composition is
 * one multiplication with no cases, and the row stays in the form everything
 * else reads. Nothing degenerates -- a zero delta or a zero current
 * orientation both fall out as the identity rather than as a division.
 */
    void RotateBy(float ax, float ay, float az, float angle)
    {
        float qw, qx, qy, qz;
        toQuat(qw, qx, qy, qz);

        const float m = std::sqrt(ax * ax + ay * ay + az * az);
        if (!(m > 0.0f) || angle == 0.0f) return;
        const float h = angle * 0.5f, sh = std::sin(h);
        const float dw = std::cos(h), dx = (ax / m) * sh, dy = (ay / m) * sh, dz = (az / m) * sh;

        const float rw = dw * qw - dx * qx - dy * qy - dz * qz;
        const float rx = dw * qx + dx * qw + dy * qz - dz * qy;
        const float ry = dw * qy - dx * qz + dy * qw + dz * qx;
        const float rz = dw * qz + dx * qy - dy * qx + dz * qw;
        fromQuat(rw, rx, ry, rz);
    }

    // Rotate a direction by this orientation -- Rodrigues, because the row is
    // already axis-angle and converting to a matrix to use it once is work
    // nobody needs. Unrotated passes the vector through untouched.
    void RotateVector(float& vx, float& vy, float& vz) const
    {
        if (!Oriented() || theta == 0.0f) return;
        const float c = std::cos(theta), s2 = std::sin(theta);
        const float dot = sx * vx + sy * vy + sz * vz;
        const float cx = sy * vz - sz * vy;
        const float cy = sz * vx - sx * vz;
        const float cz = sx * vy - sy * vx;
        const float rx = vx * c + cx * s2 + sx * dot * (1.0f - c);
        const float ry = vy * c + cy * s2 + sy * dot * (1.0f - c);
        const float rz = vz * c + cz * s2 + sz * dot * (1.0f - c);
        vx = rx; vy = ry; vz = rz;
    }

    // Rotate a point of this vector's frame about its own pivot. The two
    // together are what row 2 exists for: an orientation with nowhere to turn
    // about is only half an answer.
    void RotateAboutPivot(float& px, float& py, float& pz) const
    {
        float rx = px - fx, ry = py - fy, rz = pz - fz;
        RotateVector(rx, ry, rz);
        px = fx + rx; py = fy + ry; pz = fz + rz;
    }

    // ── aggregation: row 2's fourth component, and what it implies ──────

    bool IsLeaf()      const { return radius <= 0.0f; }
    bool IsAggregate() const { return radius >  0.0f; }

    /*
 * Reduce a set of members into this vector.
 *
 * THE MEMBERS MAY THEMSELVES BE AGGREGATES, which is the whole reason this
 * lives on OrderVector rather than on a separate type: a zone over zones and
 * a zone over points are the same call, and the recursion terminates at
 * members whose radius is zero. A member's own radius extends this one's
 * reach, so a coarse level built over a fine one is exact rather than a
 * bounding box of bounding boxes that has quietly lost the guarantee.
 *
 * POSITION IS THE ENERGY-WEIGHTED CENTRE and the PIVOT IS THE GEOMETRIC ONE,
 * which is not a redundancy: where a composition's energy is and where its
 * shape balances are different questions, and rotation is about the second.
 * They coincide exactly when the energy is spread evenly, which is the case
 * that makes the distinction easy to miss.
 *
 * ENERGY AND THE KINETIC VECTOR ADD. Summing K and dividing by the summed E
 * keeps |O| <= 1 through the reduction with no clamping: a sum of vectors
 * each no longer than its own energy is no longer than the total.
 *
 * WHAT IS NOT REDUCED, stated rather than approximated: the orientation.
 * Members' angles are relative to what contains them, so a parent's own angle
 * is its own and is not derivable from theirs. The ORBITAL contribution of
 * members circulating about the pivot is likewise absent -- it is real
 * angular energy and it needs the rate row that row 3 does not yet carry. A
 * reduction done today is exact about where, how much and how far, and silent
 * about spin.
 */
    void Reduce(const OrderVector* members, size_t n)
    {
        radius = 0.0f;
        x = y = z = 0.0f;
        ox = oy = oz = 0.0f;
        energy = 0.0f;
        if (!members || n == 0) return;

        float kx = 0.0f, ky = 0.0f, kz = 0.0f;
        float gx = 0.0f, gy = 0.0f, gz = 0.0f;   // geometric centre
        for (size_t i = 0; i < n; ++i)
        {
            const OrderVector& m = members[i];
            energy += m.energy;
            x += m.x * m.energy; y += m.y * m.energy; z += m.z * m.energy;
            gx += m.x; gy += m.y; gz += m.z;
            kx += m.ox * m.energy; ky += m.oy * m.energy; kz += m.oz * m.energy;
        }

        const float inv_n = 1.0f / static_cast<float>(n);
        gx *= inv_n; gy *= inv_n; gz *= inv_n;

        if (energy > 0.0f) { x /= energy; y /= energy; z /= energy; }
        else               { x = gx; y = gy; z = gz; }   // no energy: shape is all there is

        SetPivot(gx - x, gy - y, gz - z);

        if (energy > 0.0f) { ox = kx / energy; oy = ky / energy; oz = kz / energy; }

        Cover(members, n);
    }

    /*
 * Set just the radius, about whatever position this vector already has.
 *
 * The half of Reduce that is wanted on its own whenever the parent's position
 * is fixed by something other than its contents -- a scene root placed by a
 * script, or moved by input, whose reach still has to cover what is under it.
 * Reducing the position there would move the thing every time its contents
 * shifted, which is the opposite of what a container is for.
 */
    void Cover(const OrderVector* members, size_t n)
    {
        radius = 0.0f;
        if (!members || n == 0) return;
        for (size_t i = 0; i < n; ++i)
        {
            const float dx = members[i].x - x;
            const float dy = members[i].y - y;
            const float dz = members[i].z - z;
            const float reach = std::sqrt(dx * dx + dy * dy + dz * dz) + members[i].radius;
            if (reach > radius) radius = reach;
        }
    }

    // Is that point inside this vector's reach at all? The coarse question,
    // answered in one comparison -- which is the whole reason a coarse level
    // exists. Always false for a leaf, which is correct: a point encloses
    // nothing, including itself.
    bool Encloses(float px, float py, float pz) const
    {
        const float dx = px - x, dy = py - y, dz = pz - z;
        return dx * dx + dy * dy + dz * dz <= radius * radius;
    }

    /*
 * Could anything under this vector have interacted with anything under that
 * one?
 *
 * Centre distance minus both radii: negative or zero means the two overlap
 * and the question has to be asked of their members; positive is the GAP, and
 * a positive gap is a proof about every pair inside, obtained without looking
 * at any of them. That asymmetry -- cheap certainty about absence, referral on
 * presence -- is what a causal-distance check is for, and it works unchanged
 * between two leaves, two aggregates, or one of each, because the radius of a
 * leaf is zero and the arithmetic does not care.
 *
 * WHICH IS WHAT MAKES LoD FREE. Several different aggregate SETS can be built
 * over the same leaves -- coarse ones with few large reaches, fine ones with
 * many small -- and they are not approximations of each other, they are
 * different reductions of the same points. Picking a set is picking the scope
 * at which causal distance is being checked, and both are exact at their own
 * scope because both reduce the same leaves. Nothing is rebuilt to change
 * scope, because nothing was approximated to build either.
 */
    float GapTo(const OrderVector& other) const
    {
        const float dx = other.x - x, dy = other.y - y, dz = other.z - z;
        return std::sqrt(dx * dx + dy * dy + dz * dz) - radius - other.radius;
    }
    bool MayInteractWith(const OrderVector& other) const { return GapTo(other) <= 0.0f; }

    /*
 * The uncertainty a crossing carries, derived from the crossing itself.
 *
 * Identity, quantity and span -- the three facts that make this crossing THIS
 * crossing -- mixed by a splitmix finaliser. A HASH AND NOT A GENERATOR:
 * there is no state to advance, because a stream position would be the second
 * clock this whole reading exists to avoid (see the header). Quantity and
 * span go in by their bit patterns rather than rounded, so two crossings that
 * differ at all differ everywhere.
 *
 * Static and taking the vector, rather than reading members, because it is a
 * function OF an emission and is worth being able to check against one
 * without having produced it.
 */
    static uint64_t derive_uncertainty(const OrderVector& e)
    {
        uint32_t qb, ib;
        std::memcpy(&qb, &e.energy,   sizeof(qb));
        std::memcpy(&ib, &e.interval, sizeof(ib));
        uint64_t h = e.rid ^ (static_cast<uint64_t>(qb) << 32) ^ ib;
        h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ull;
        h ^= h >> 27; h *= 0x94d049bb133111ebull;
        h ^= h >> 31;
        return h;
    }

private:
    void toQuat(float& w, float& x, float& y, float& z) const
    {
        if (!Oriented() || theta == 0.0f) { w = 1.0f; x = y = z = 0.0f; return; }
        const float h = theta * 0.5f, sh = std::sin(h);
        w = std::cos(h); x = sx * sh; y = sy * sh; z = sz * sh;
    }
    void fromQuat(float w, float x, float y, float z)
    {
        const float n = std::sqrt(w * w + x * x + y * y + z * z);
        if (!(n > 0.0f)) { sx = sy = sz = 0.0f; theta = 0.0f; return; }
        w /= n; x /= n; y /= n; z /= n;
        if (w >  1.0f) w =  1.0f;
        if (w < -1.0f) w = -1.0f;
        const float half = std::acos(w);
        const float s2   = std::sqrt(1.0f - w * w);
        if (!(s2 > 1e-7f)) { sx = sy = sz = 0.0f; theta = 0.0f; return; }  // identity
        sx = x / s2; sy = y / s2; sz = z / s2;
        theta = half * 2.0f;
    }

    // O is K scaled by 1/E, which is the whole of the linear relation between
    // the fraction and the energy. E of zero has no direction to record, so the
    // point comes to rest rather than dividing.
    void setKinetic(float kx, float ky, float kz)
    {
        if (!(energy > 0.0f)) { Rest(); return; }
        ox = kx / energy; oy = ky / energy; oz = kz / energy;
    }
};

#endif
