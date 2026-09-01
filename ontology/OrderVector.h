#ifndef ONTOLOGY_ORDERVECTOR_H__
#define ONTOLOGY_ORDERVECTOR_H__


#include "../core_defs.h"
#include <cmath>
#include <cstdint>

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
// AND THAT IS THE SAME COUNTER A PRNG DRAW ADVANCES. A random number is not a
// separate mechanism that happens to also need a clock -- it IS an entropy
// emission, the uncertainty of one boundary crossing written down, so an
// entity drawing from a substituted PRNG and an entity shedding heat are
// ticking the one counter for the same reason. Anything that has to be
// deterministic per entity (a sandbox substituting the generator, a replay
// that must land on the same draws) indexes off this count rather than a
// stream position of its own: two clocks would have to be kept in step, and
// keeping them in step is exactly the thing that cannot be done from inside a
// sandbox. Reserved here rather than implemented -- the counter is on the
// bodies (Scene3D::CausalTicks) and the substitution is not written yet -- but
// the reading is fixed now, because it is the part that decides whether the
// two mechanisms can ever be one.
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
// RESERVED: the other two rows.
//
// This is row 0 and row 1 of a 4x4 that describes ONE SPHERE -- one
// distinguishable unit of space -- with a constraint set over its rows:
//
//     row 2   (Px, Py, Pz, r)        rotational pivot, and the bounding radius
//     row 3   (Sx, Sy, Sz, phi)      spinor / angle causal relation
//
// Row 2 makes rotation a relation to a pivot rather than a property of a body,
// which is what lets a sub-unit rotate about a parent's pivot without holding a
// copy of it. Row 3 carries the angular half of what rows 0 and 1 carry
// linearly, on the same reading: a direction and a fraction, of the same E.
//
// A SPHERE WITH A RADIUS IS ALWAYS AN AGGREGATE. r > 0 means the unit is
// composed of sub-units, and the recursion bottoms out at leaves with r = 0 --
// points, which are the only things that are not aggregates of anything. So a
// radius is not a size, it is a statement about how far down the composition
// this row has summarised, and every quantity on a sphere with r > 0 is a
// reduction over its leaves rather than an independent value that could
// disagree with them.
//
// WHICH IS WHAT MAKES LoD FREE. Several different sphere SETS can be built over
// the same leaves -- coarse ones with few large spheres, fine ones with many
// small -- and they are not approximations of each other, they are different
// reductions of the same points. Picking a set is picking the scope at which
// causal distance is being checked: a coarse set answers "could these two
// regions interact at all" in a handful of comparisons, a fine one answers
// "which leaves actually did", and both are exact at their own scope because
// both reduce the same leaves.
//
// None of rows 2 and 3 is implemented here. The two rows below are, the
// invariant they maintain is, and the fourth-column reading the later rows have
// to honour is -- which is the part that is expensive to change later and free
// to get right now.
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

private:
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
