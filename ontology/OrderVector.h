#ifndef ONTOLOGY_ORDERVECTOR_H__
#define ONTOLOGY_ORDERVECTOR_H__

#include "../core_defs.h"
#include "Fixed.h"
#include "Mat.h"
#include <cmath>
#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------
// OrderVector — four rows of four, each a distinguishable unit of space, and
// the one causal statement of where and how a thing is.
//
//     row 0   (x,  y,  z,  id)       where it is, and what thing it is
//     row 1   (Ox, Oy, Oz, E)        where its energy is going, and how much
//     row 2   (Fx, Fy, Fz, r)        what it turns about, and how far it reaches
//     row 3   (qx, qy, qz, qw)       which way it faces -- a unit quaternion
//
// THE ROWS ARE INTEGERS (Fixed.h, Q32.32). This vector is the physics: it is
// what an interaction is gated on (GapTo), what an emission's uncertainty is
// derived from, and what a history is. Two runtimes replaying the same lines
// have to reach the same rows bit for bit, on a laptop and in a browser, and
// a float cannot promise that across libm implementations and fused
// multiplies. So the rows are integers and every operation on them is
// integer arithmetic; the float appears in exactly one place, ToMatrix4,
// which is the PROJECTION of this record into a picture -- and a projection
// is something a runtime can decline to make and still run all of the logic,
// faster, with nothing on screen. That is what the split is for.
//
// ROW 0'S FOURTH SLOT IS THE IDENTITY TUPLE: what the thing is, then which
// of the indistinguishable ones. The entity's identity hash (core/Entity.h,
// identityHash: the merkle over tags, relations, dispatch and children -- no
// RIDs, no values) mixed with its index among its twins (siblingIndex: the
// parent's children of the same tag and the same identity hash, in attach
// order), set by the family base whenever either moves. A RID is the order
// types happened to be created in; the tuple's last key is creation order
// too, but only between twins, where it is the one fact left -- the same two
// keys a persistence record is looked up by (ontology/Environmental.h). Two
// units at the same position are still two units.
//
// ROW 1 IS A DIRECTION AND A FRACTION AT THE SAME TIME. O is not a velocity:
// |O| lies in [0,1] and is the SHARE of E that is kinetic along O/|O|.
//
//     kinetic vector K = O * E        kinetic energy |K| = |O| * E
//     heat E - |K| = (1 - |O|) * E    invariant |O| <= 1
//
// Drag is then a TRANSFER rather than an erasure: Dissipate scales |K| down
// and leaves E alone, so what left the motion is exactly what arrived as heat,
// and nothing had to be subtracted from anywhere. The accounting is the
// representation.
//
// THE ARROW OF TIME IS STRUCTURAL: |K| never rises except through work done
// from outside -- Impulse, or a field's Accelerate. Dissipate lowers it, Emit
// and Advance hold it, and a contact's SetVelocity can only spend what is
// already there. Ordered
// energy decays into unordered and never the other way, which is the second
// law as a property of the operation set. (Emit raises |O| while holding |K|,
// and that is not a counterexample: heat leaving shrinks the total, so the
// surviving motion is a larger share of a smaller number.)
//
// EMISSION IS THE CLOCK. Each commit of entropy is one tick of this point's
// own time: an entity shedding entropy quickly runs fast, one at rest with no
// heat left emits nothing and its counter stops -- nothing distinguishes one
// of its moments from the next, so it has no time to keep. A wall clock is
// used only to work out how much is OWED at an interaction; what an entity's
// history is measured in is how many times it has paid. Nothing is ordered
// inside one emission: a draw nested in it reads the emission's own
// uncertainty (derive_uncertainty), which is a hash of the crossing and not a
// generator with a position -- so replaying the same emission yields the same
// draw with no seed threaded anywhere.
//
// AN EMISSION IS ITSELF AN OrderVector: a quantity of energy AT a place FROM
// an identity -- rows 0 and 1 exactly -- with |O| = 0, since what crossed was
// entropy. EmitEvent hands it out, Absorb takes it in, and a crossing that
// carried a direction would be radiation pressure, already representable.
//
// ROW 2: the pivot is in this vector's OWN frame, relative to row 0, so a
// sub-unit turning about its parent's pivot holds no copy of it. The fourth
// slot is the REACH: zero is a point, positive says this vector summarises a
// set that reaches that far -- one type, and a leaf is the case where the
// radius is zero (Reduce over one member returns it unchanged).
//
// ROW 3 IS THE SPINOR, and it fills its four slots now. Axis-angle was the
// reading; a unit quaternion is the representation, because composing two
// rotations is then a handful of multiplies with no trigonometry in it, and
// "turn from where you are" is one multiplication with no gimbal and no
// degenerate case. The angle is still asked for as an axis and an angle
// (Orient, RotateBy) -- sin and cos of the half angle are computed ONCE at
// that boundary by Fixed's own series -- and the identity is (0,0,0,1) with
// no sentinel. Angular RATE -- the share of E that is rotational -- is still
// deliberately absent: the linear rows carry causality for now.
//
// THE INVARIANT IS STRUCTURAL, AND STATED WITH ITS SLACK. Impulse adds j
// joules along a unit direction: |K| grows by AT MOST j while E grows by
// exactly j, so |O| <= 1 survives every impulse; Dissipate can only lower it.
// No sequence of the operations below leaves a point with more kinetic energy
// than energy -- up to the last bits of a truncating divide, which is why
// Holds() carries an epsilon and the tester fuzzes the operation set against
// it (loaders/OrderVectorTesterLoader.cc) rather than this comment standing
// alone. The constraints the family upholds, and which of them this struct
// is responsible for, are listed in ontology/etcs_causal_constraints.md.
//
// The two meta fields are properties of the causal step this vector
// represents -- the span it settles and the uncertainty a draw inside it
// reads -- and live here rather than in a companion struct because a
// companion is a second thing that can disagree with the first. On a BODY
// they describe its most recent crossing; on an EMISSION they describe that
// emission.
// ---------------------------------------------------------------------------
struct OrderVector
{
    // ── row 0: the point ────────────────────────────────────────────────
    // The identity in the fourth slot: the state hash of the thing whose rows
    // these are (see above). Zero until the family base has set it.
    Fixed    x, y, z;
    uint64_t id = 0;

    // ── row 1: the order ────────────────────────────────────────────────
    Fixed ox, oy, oz, energy;

    // ── row 2: the pivot, and how far this vector reaches ───────────────
    Fixed fx, fy, fz, radius;

    // ── row 3: the spinor ───────────────────────────────────────────────
    Fixed qx, qy, qz, qw = Fixed::One();

    // ── meta: the step ──────────────────────────────────────────────────
    Fixed    interval;
    uint64_t uncertainty = 0;

    // ── readings of the relation ────────────────────────────────────────

    /*
     * THE 4x4 THIS VECTOR IMPLIES -- the projection, and the one float here.
     *
     * Built rather than reinterpreted: row 0's fourth slot is a RID and no
     * product should touch it, so the renderer's matrix is a FUNCTION of this
     * vector. Translation from row 0, rotation from row 3 (a unit quaternion
     * to a matrix needs no trigonometry). Radius, energy and pivot are not
     * placement and do not appear.
     */
    Matrix4 ToMatrix4() const
    {
        Matrix4 m = Matrix4::Identity();
        const float X = qx.ToFloat(), Y = qy.ToFloat(), Z = qz.ToFloat(), W = qw.ToFloat();
        m.at(0,0) = 1.0f - 2.0f*(Y*Y + Z*Z); m.at(0,1) = 2.0f*(X*Y - Z*W);       m.at(0,2) = 2.0f*(X*Z + Y*W);
        m.at(1,0) = 2.0f*(X*Y + Z*W);       m.at(1,1) = 1.0f - 2.0f*(X*X + Z*Z); m.at(1,2) = 2.0f*(Y*Z - X*W);
        m.at(2,0) = 2.0f*(X*Z - Y*W);       m.at(2,1) = 2.0f*(Y*Z + X*W);       m.at(2,2) = 1.0f - 2.0f*(X*X + Y*Y);
        m.at(0,3) = x.ToFloat(); m.at(1,3) = y.ToFloat(); m.at(2,3) = z.ToFloat();
        return m;
    }

    Fixed KineticFraction() const { return Fixed::Length(ox, oy, oz); }
    // |K| as the length of K = O * E, not |O| * E: the three products lose a
    // last bit each either way, but a root taken of |O|^2 first amplifies
    // that loss by 1/(2|O|) and the energy multiplies it after -- a nearly
    // still body of a thousand joules read its ordered energy with an error
    // a thousand times the slack the invariants allow. Taken this way the
    // error is a few last bits, whatever |O| is.
    Fixed KineticEnergy()   const { return Fixed::Length(ox * energy, oy * energy, oz * energy); }
    Fixed Heat()            const { return energy - KineticEnergy(); }

    // The direction of travel, or zero when there is none -- the honest
    // answer for a point whose energy is all heat.
    void Direction(Fixed& dx, Fixed& dy, Fixed& dz) const { directionGiven(KineticFraction(), dx, dy, dz); }

    // Speed from kinetic energy: |K| = 1/2 m v^2. Mass is a parameter because
    // it belongs to the body, not the point: a leaf and the sphere
    // aggregating it have the same accounting and different masses. One
    // root for |O| and one for the speed; the direction reuses the first.
    void Velocity(Fixed mass, Fixed& vx, Fixed& vy, Fixed& vz) const
    {
        vx = vy = vz = Fixed::Zero();
        if (!mass.IsPositive()) return;
        const Fixed m  = KineticFraction();
        const Fixed ke = KineticEnergy();
        if (!ke.IsPositive()) return;
        Fixed dx, dy, dz;
        directionGiven(m, dx, dy, dz);
        const Fixed v = (Fixed::FromInt(2) * ke / mass).Sqrt();
        vx = dx * v; vy = dy * v; vz = dz * v;
    }

    // ── the operations ──────────────────────────────────────────────────

    // Add `joules` of kinetic energy along a direction (normalised here):
    // E grows by exactly j, K by at most j, and pushing against the current
    // motion lands the difference in heat by construction.
    void Impulse(Fixed dx, Fixed dy, Fixed dz, Fixed joules)
    {
        if (!joules.IsPositive()) return;
        const Fixed m = Fixed::Length(dx, dy, dz);
        if (!m.IsPositive()) return;
        Fixed kx = ox * energy, ky = oy * energy, kz = oz * energy;
        kx += (dx / m) * joules;
        ky += (dy / m) * joules;
        kz += (dz / m) * joules;
        energy += joules;
        setKinetic(kx, ky, kz);
    }

    /*
     * A FIELD'S WORK: the velocity changes by a*dt (a gravity, a slope's pull)
     * and E moves by exactly what the kinetic energy did -- up when the field
     * speeds the body, down when the body climbs against it. Returned, signed:
     * the energy the field put in, which its source counts
     * (CausalBase::CountFieldWorkUnder) so the ledger stays exact. Beside
     * Impulse, the other way work is done from outside; the one that can also
     * take it back out.
     */
    Fixed Accelerate(Fixed ax, Fixed ay, Fixed az, Fixed dt, Fixed mass)
    {
        if (!dt.IsPositive() || !mass.IsPositive()) return Fixed::Zero();
        Fixed vx, vy, vz;
        Velocity(mass, vx, vy, vz);
        const Fixed before = KineticEnergy();
        vx += ax * dt; vy += ay * dt; vz += az * dt;
        const Fixed v = Fixed::Length(vx, vy, vz);
        const Fixed after = Fixed::Half() * mass * v * v;
        Fixed work = after - before;
        if ((energy + work).raw < 0) work = -energy;   // cannot give back more than it holds
        energy += work;
        if (!v.IsPositive() || !after.IsPositive()) { Rest(); return work; }
        const Fixed ke = Fixed::Min(after, energy);
        setKinetic((vx / v) * ke, (vy / v) * ke, (vz / v) * ke);
        return work;
    }

    /*
     * THE MOTION OUTRIGHT, as a velocity: K is set to 1/2 m |v|^2 along v and E
     * is left alone, so a motion that lost speed (a bounce, a rub) put the
     * difference into heat by construction. A motion that GAINS must have its
     * energy arrive first (Absorb, as heat) -- capped at E here, so the row
     * holds whatever the caller did. Returns the kinetic change.
     */
    Fixed SetVelocity(Fixed mass, Fixed vx, Fixed vy, Fixed vz)
    {
        const Fixed before = KineticEnergy();
        const Fixed v = Fixed::Length(vx, vy, vz);
        if (!mass.IsPositive() || !v.IsPositive()) { Rest(); return -before; }
        const Fixed ke = Fixed::Min(Fixed::Half() * mass * v * v, energy);
        setKinetic((vx / v) * ke, (vy / v) * ke, (vz / v) * ke);
        return ke - before;
    }

    // Scale the kinetic share by `factor` in [0,1) and leave E alone: drag,
    // friction, inelastic contact. The difference IS heat, by definition.
    void Dissipate(Fixed factor)
    {
        if (factor.raw < 0) factor = Fixed::Zero();
        if (factor >= Fixed::One()) return;
        ox *= factor; oy *= factor; oz *= factor;
    }

    /*
     * ENTROPY EMISSION: how much heat leaves over dt at this emissivity,
     * h * (1 - exp(-k*dt)) -- exponential in the heat held, which is what
     * makes committing it LAZILY exact: once for an interval T is identical
     * to N times across the same T, so a point nobody looked at for a while
     * owes exactly one calculation. That holds only while nothing else
     * touches the heat during the interval, which is why the commit belongs
     * at the same instant as the drag.
     */
    Fixed EmissionOver(Fixed dt, Fixed emissivity) const { return Heat() * EmissionShare(dt, emissivity); }

    // The share of the heat that leaves over dt at this emissivity,
    // 1 - exp(-k dt): a function of the two alone, so a driver stepping at
    // one rate computes it once (CausalBase::CommitEntropy) and the series
    // is not run per tick.
    static Fixed EmissionShare(Fixed dt, Fixed emissivity)
    {
        if (!dt.IsPositive() || !emissivity.IsPositive()) return Fixed::Zero();
        return Fixed::One() - (-(emissivity * dt)).Exp();
    }

    // Take heat out of the point and return what actually left, capped at the
    // heat there is. E falls, K is held, so |O| rises. Emit and Absorb are the
    // same operation with opposite sign.
    Fixed Emit(Fixed joules) { return emitGiven(Heat(), joules); }

    // Emit as the EVENT: the crossing, as the OrderVector it is -- where it
    // left, whose boundary, how much, all of it unordered, and the
    // uncertainty a draw nested inside it reads. Nothing is stored; a zero
    // emission is not an event.
    OrderVector EmitEvent(Fixed joules, Fixed span) { return emitEventGiven(Heat(), joules, span); }

    // The whole commit in one reading of the heat: that share of it, taken
    // out, as the event. EmissionOver + EmitEvent read the heat twice; a
    // driver doing this thousands of times a second reads it once.
    //
    // THE LAST QUANTUM LEAVES WHOLE: once the heat is so small that its share
    // truncates to nothing, the whole of it goes. Otherwise a body would hold
    // a residual it could never emit, with its clock stopped while it still
    // had heat -- and "no heat left" (the clock stopping) is meant literally.
    OrderVector CommitShare(Fixed share, Fixed span)
    {
        const Fixed h = Heat();
        Fixed q = h.IsPositive() ? h * share : Fixed::Zero();
        if (h.IsPositive() && share.IsPositive() && !q.IsPositive()) q = h;
        return emitEventGiven(h, q, span);
    }

    /*
     * A CONTACT CROSSING: the part of this point's ordered energy that is
     * headed along n (toward the thing it touched) leaves, as an emission
     * that carries that direction -- all of it ordered, |O| = 1 -- for the
     * other side to absorb as an impulse. What stays is the rest of the
     * kinetic energy, along what was perpendicular to n. With v the velocity
     * and c = cos of the angle between v and n: KE * c^2 crosses, KE * (1 -
     * c^2) stays, so a head-on contact hands everything over and a glancing
     * one almost nothing. Nothing crosses when the motion is away from n.
     *
     * This is transmission, not restitution: the energy along the line goes
     * to the other body entirely (Newton's cradle for equal masses). It is
     * exact in the ledger -- E falls by what the event carries, |K| falls by
     * at least what the absorber's |K| can rise by -- and it is the same
     * crossing a member's heat makes into its container, with a direction.
     * A coefficient of restitution is a later parameter on this one
     * operation, not a second one.
     */
    OrderVector CrossToward(Fixed nx, Fixed ny, Fixed nz, Fixed span)
    {
        OrderVector e;
        e.x = x; e.y = y; e.z = z;
        e.id = id;
        e.interval = span;
        const Fixed m  = KineticFraction();
        const Fixed ke = KineticEnergy();
        const Fixed nl = Fixed::Length(nx, ny, nz);
        if (!ke.IsPositive() || !nl.IsPositive()) return e;
        const Fixed ux = nx / nl, uy = ny / nl, uz = nz / nl;      // n, unit
        const Fixed c  = (ox * ux + oy * uy + oz * uz) / m;         // v^ . n^
        if (!c.IsPositive()) return e;                              // not toward it
        const Fixed a  = Fixed::Min(ke * c * c, ke);                // what crosses
        // What stays, along the perpendicular of v^ to n^.
        const Fixed px = ox / m - c * ux, py = oy / m - c * uy, pz = oz / m - c * uz;
        const Fixed pl = Fixed::Length(px, py, pz);
        const Fixed rest = ke - a;
        Fixed kx, ky, kz;
        if (rest.IsPositive() && pl.IsPositive()) { kx = (px / pl) * rest; ky = (py / pl) * rest; kz = (pz / pl) * rest; }
        energy -= a;
        setKinetic(kx, ky, kz);
        e.energy = a;
        e.ox = ux; e.oy = uy; e.oz = uz;
        e.uncertainty = derive_uncertainty(e);
        return e;
    }

    // Heat in, motion untouched: an environment absorbing its contents'
    // entropy gets warmer and does not start moving.
    void Absorb(Fixed joules)
    {
        if (!joules.IsPositive()) return;
        const Fixed kx = ox * energy, ky = oy * energy, kz = oz * energy;
        energy += joules;
        setKinetic(kx, ky, kz);
    }

    // Absorb a whole crossing: its heat lands as heat, its ordered part as an
    // impulse (radiation pressure, with no new operation). The crossing's
    // meta is NOT copied -- that step was somebody else's clock.
    //
    // EXACTLY ITS ENERGY, by construction: the ordered part is read once and
    // capped at the whole, and the heat is the remainder, so the two adds
    // sum to q.energy to the bit -- the ledger (etcs_causal_constraints.md
    // §3) does not depend on |K| of a unit direction being exactly 1.
    void Absorb(const OrderVector& q)
    {
        if (!q.energy.IsPositive()) return;
        const Fixed ke   = Fixed::Min(q.KineticEnergy(), q.energy);
        const Fixed heat = q.energy - ke;
        if (heat.IsPositive()) Absorb(heat);
        if (ke.IsPositive())
        {
            Fixed dx, dy, dz;
            q.Direction(dx, dy, dz);
            Impulse(dx, dy, dz, ke);
        }
    }

    // Move the point by one step of its own rate row -- the only place row 1
    // touches row 0.
    void Advance(Fixed dt, Fixed mass)
    {
        if (!dt.IsPositive()) return;
        Fixed vx, vy, vz;
        Velocity(mass, vx, vy, vz);
        AdvanceBy(vx, vy, vz, dt);
    }
    // The same step with the velocity already read -- for a caller that has
    // just read it to decide whether to step at all (Scene3D::stepOver).
    void AdvanceBy(Fixed vx, Fixed vy, Fixed vz, Fixed dt)
    {
        if (!dt.IsPositive()) return;
        x += vx * dt; y += vy * dt; z += vz * dt;
    }

    // A teleport: not a motion, it changes where the point carries what it
    // carries and nothing about what it carries.
    void PlaceAt(Fixed px, Fixed py, Fixed pz) { x = px; y = py; z = pz; }

    // All energy to heat, motion to rest: a collision with something
    // immovable, a released control eventually.
    void Rest() { ox = oy = oz = Fixed::Zero(); }

    // ── row 2 / row 3 ───────────────────────────────────────────────────

    void SetPivot(Fixed px, Fixed py, Fixed pz) { fx = px; fy = py; fz = pz; }
    bool Oriented() const { return !qx.IsZero() || !qy.IsZero() || !qz.IsZero(); }

    // The orientation outright, from an axis (any non-zero vector) and an
    // angle in radians. The one place trigonometry is asked for, at the
    // boundary where an angle comes in; the row stores the spinor.
    void Orient(Fixed ax, Fixed ay, Fixed az, Fixed angle)
    {
        Fixed w, X, Y, Z;
        if (!halfTurn(ax, ay, az, angle, w, X, Y, Z)) { qx = qy = qz = Fixed::Zero(); qw = Fixed::One(); return; }
        qx = X; qy = Y; qz = Z; qw = w;
        renormalise();
    }

    // The orientation outright, so that this frame's up (+y) points along d:
    // the shortest turn from +y to d, built as a spinor from the two
    // directions with no angle in it (half-way vector), and a half turn about
    // x when d is straight down. A zero d leaves the facing as it was.
    void Aim(Fixed dx, Fixed dy, Fixed dz)
    {
        const Fixed l = Fixed::Length(dx, dy, dz);
        if (!l.IsPositive()) return;
        const Fixed ux = dx / l, uy = dy / l, uz = dz / l;
        const Fixed w = Fixed::One() + uy;               // 1 + (+y . d)
        if (w.raw <= (Fixed::ONE >> 20)) { qx = Fixed::One(); qy = qz = qw = Fixed::Zero(); return; }
        qx = uz; qy = Fixed::Zero(); qz = -ux; qw = w;   // (+y x d, 1 + +y . d)
        renormalise();
    }

    // Compose a delta rotation on the LEFT -- R_new = R_delta * R_current --
    // so a movement means "turn from where you are now" rather than in the
    // local frame, which is the difference between a look control that
    // behaves and one that corkscrews once pitched. One quaternion product.
    void RotateBy(Fixed ax, Fixed ay, Fixed az, Fixed angle)
    {
        Fixed dw, dx, dy, dz;
        if (!halfTurn(ax, ay, az, angle, dw, dx, dy, dz)) return;
        const Fixed rw = dw * qw - dx * qx - dy * qy - dz * qz;
        const Fixed rx = dw * qx + dx * qw + dy * qz - dz * qy;
        const Fixed ry = dw * qy - dx * qz + dy * qw + dz * qx;
        const Fixed rz = dw * qz + dx * qy - dy * qx + dz * qw;
        qw = rw; qx = rx; qy = ry; qz = rz;
        renormalise();
    }

    // Rotate a direction by this orientation: v' = v + 2w(q x v) + 2(q x (q x v)).
    void RotateVector(Fixed& vx, Fixed& vy, Fixed& vz) const
    {
        if (!Oriented()) return;
        const Fixed two = Fixed::FromInt(2);
        const Fixed tx = two * (qy * vz - qz * vy);
        const Fixed ty = two * (qz * vx - qx * vz);
        const Fixed tz = two * (qx * vy - qy * vx);
        const Fixed rx = vx + qw * tx + (qy * tz - qz * ty);
        const Fixed ry = vy + qw * ty + (qz * tx - qx * tz);
        const Fixed rz = vz + qw * tz + (qx * ty - qy * tx);
        vx = rx; vy = ry; vz = rz;
    }

    // The inverse: a direction of the frame this vector faces in, back into
    // its own (the conjugate spinor). What a solid asks to meet a point in
    // its own frame (CausalBase's solid contact).
    void UnrotateVector(Fixed& vx, Fixed& vy, Fixed& vz) const
    {
        if (!Oriented()) return;
        OrderVector c;
        c.qx = -qx; c.qy = -qy; c.qz = -qz; c.qw = qw;
        c.RotateVector(vx, vy, vz);
    }

    // Rotate a point of this vector's frame about its own pivot -- what row 2
    // exists for.
    void RotateAboutPivot(Fixed& px, Fixed& py, Fixed& pz) const
    {
        Fixed rx = px - fx, ry = py - fy, rz = pz - fz;
        RotateVector(rx, ry, rz);
        px = fx + rx; py = fy + ry; pz = fz + rz;
    }

    // ── aggregation: row 2's fourth component, and what it implies ──────

    bool IsLeaf()      const { return !radius.IsPositive(); }
    bool IsAggregate() const { return  radius.IsPositive(); }

    /*
     * Reduce a set of members into this vector. Members may be aggregates,
     * which is why this lives here: a zone over zones and a zone over points
     * are the same call. Position is the energy-weighted centre and the
     * pivot the geometric one (where the energy is and where the shape
     * balances are different questions); energy and the kinetic vector add,
     * which keeps |O| <= 1 through the reduction with no clamping. The
     * orientation is not reduced -- a parent's angle is its own -- and the
     * orbital contribution of members about the pivot needs the rate row 3
     * does not carry yet.
     */
    void Reduce(const OrderVector* members, size_t n)
    {
        radius = Fixed::Zero();
        x = y = z = Fixed::Zero();
        ox = oy = oz = Fixed::Zero();
        energy = Fixed::Zero();
        if (!members || n == 0) return;

        Fixed kx, ky, kz, gx, gy, gz;
        for (size_t i = 0; i < n; ++i)
        {
            const OrderVector& m = members[i];
            energy += m.energy;
            x += m.x * m.energy; y += m.y * m.energy; z += m.z * m.energy;
            gx += m.x; gy += m.y; gz += m.z;
            kx += m.ox * m.energy; ky += m.oy * m.energy; kz += m.oz * m.energy;
        }
        const Fixed count = Fixed::FromInt(static_cast<int64_t>(n));
        gx /= count; gy /= count; gz /= count;

        if (energy.IsPositive()) { x /= energy; y /= energy; z /= energy; }
        else                     { x = gx; y = gy; z = gz; }   // no energy: shape is all there is

        SetPivot(gx - x, gy - y, gz - z);
        if (energy.IsPositive()) { ox = kx / energy; oy = ky / energy; oz = kz / energy; }
        Cover(members, n);
    }

    // Just the radius, about whatever position this vector already has: for a
    // container placed by something other than its contents.
    void Cover(const OrderVector* members, size_t n)
    {
        radius = Fixed::Zero();
        if (!members || n == 0) return;
        for (size_t i = 0; i < n; ++i)
        {
            const Fixed reach = Fixed::Length(members[i].x - x, members[i].y - y, members[i].z - z) + members[i].radius;
            if (reach > radius) radius = reach;
        }
    }

    // Is that point inside this vector's reach? Always false for a leaf: a
    // point encloses nothing, including itself.
    bool Encloses(Fixed px, Fixed py, Fixed pz) const
    {
        if (!radius.IsPositive()) return false;
        const Fixed dx = px - x, dy = py - y, dz = pz - z;
        return dx * dx + dy * dy + dz * dz <= radius * radius;
    }

    /*
     * Centre distance minus both radii: a positive GAP is a proof about every
     * pair inside, obtained without looking at any of them; zero or negative
     * refers the question to the members. Unchanged between two leaves, two
     * aggregates, or one of each. Different aggregate sets over the same
     * leaves are different exact reductions, not approximations of each
     * other, which is what makes level of detail free.
     */
    Fixed GapTo(const OrderVector& other) const
    {
        return Fixed::Length(other.x - x, other.y - y, other.z - z) - radius - other.radius;
    }
    // GapTo <= 0 without the root: the gate is asked of every pair of a
    // container's members every interaction, and squares compare the same.
    // Each square is floored to Fixed as a Fixed product is, then summed in
    // 96+ bits: in Fixed the squares wrapped past ~46k units apart and a far
    // pair could pass. Where nothing wrapped, the same answer to the bit.
    bool MayInteractWith(const OrderVector& other) const
    {
        Wide s = Wide::SquareOf(Diff(other.x.raw, x.raw));
        s += Wide::SquareOf(Diff(other.y.raw, y.raw));
        s += Wide::SquareOf(Diff(other.z.raw, z.raw));
        return s <= Wide::SquareOf(Sum(radius.raw, other.radius.raw));
    }

    // Does this vector's reach lie inside a sphere of radius `space` about
    // (cx, cy, cz)? |p - c| + reach <= space, exactly as the gate compares:
    // floored squares, summed wide. What a container's fitness asks of a
    // member (CausalBase::fitLocked): whether it is in the space provided.
    bool InsideOf(Fixed cx, Fixed cy, Fixed cz, Fixed space) const
    {
        const Fixed room = space - radius;
        if (room.raw < 0) return false;
        Wide s = Wide::SquareOf(Diff(x.raw, cx.raw));
        s += Wide::SquareOf(Diff(y.raw, cy.raw));
        s += Wide::SquareOf(Diff(z.raw, cz.raw));
        return s <= Wide::SquareOf(static_cast<uint64_t>(room.raw));
    }

private:
    // An unsigned 128-bit number in halves (no __int128 on every target, as
    // Fixed's own arithmetic). Only what the gate needs.
    struct Wide
    {
        uint64_t hi = 0, lo = 0;
        // floor(m^2 / 2^32): what Fixed's product keeps of a square.
        static Wide SquareOf(uint64_t m)
        {
            uint64_t h, l;
            fixed_detail::umul(m, m, h, l);
            return Wide{ h >> 32, (h << 32) | (l >> 32) };
        }
        Wide& operator+=(const Wide& o)
        {
            const uint64_t l = lo + o.lo;
            hi += o.hi + (l < lo ? 1 : 0);
            lo = l;
            return *this;
        }
        bool operator<=(const Wide& o) const { return hi != o.hi ? hi < o.hi : lo <= o.lo; }
    };
    // |a - b| and |a + b| without wrapping: both fit 64 bits unsigned (the
    // sum's one exception, two minima, saturates).
    static uint64_t Diff(int64_t a, int64_t b)
    {
        return a >= b ? static_cast<uint64_t>(a) - static_cast<uint64_t>(b)
                      : static_cast<uint64_t>(b) - static_cast<uint64_t>(a);
    }
    static uint64_t Sum(int64_t a, int64_t b)
    {
        if ((a < 0) != (b < 0))
        {
            const int64_t s = a + b;   // opposite signs: cannot overflow
            return s < 0 ? 0 - static_cast<uint64_t>(s) : static_cast<uint64_t>(s);
        }
        const uint64_t ma = a < 0 ? 0 - static_cast<uint64_t>(a) : static_cast<uint64_t>(a);
        const uint64_t mb = b < 0 ? 0 - static_cast<uint64_t>(b) : static_cast<uint64_t>(b);
        return ma > ~mb ? ~uint64_t{0} : ma + mb;
    }

public:

    // The uncertainty a crossing carries, derived from the crossing itself:
    // where it left, from what, how much, which way, and over what span,
    // through a finaliser. A hash, not a generator -- no state to advance.
    // Everything goes in as the integers they are, so two crossings that
    // differ at all differ everywhere, and the same on every platform. No
    // creation order anywhere in it (row 0's identity is the state hash).
    static uint64_t derive_uncertainty(const OrderVector& e)
    {
        uint64_t h = fixed_mix(e.id, e.x.raw);
        h = fixed_mix(h, e.y.raw);  h = fixed_mix(h, e.z.raw);
        h = fixed_mix(h, e.ox.raw); h = fixed_mix(h, e.oy.raw); h = fixed_mix(h, e.oz.raw);
        h = fixed_mix(h, e.energy.raw);
        return fixed_mix(h, e.interval.raw);
    }

    // Every row, as one number: what two runtimes compare to say they have
    // the same history. The meta is included -- it is the step's state.
    uint64_t Hash() const
    {
        uint64_t h = 0x243f6a8885a308d3ull;
        const int64_t words[] = { x.raw, y.raw, z.raw, static_cast<int64_t>(id),
                                  ox.raw, oy.raw, oz.raw, energy.raw,
                                  fx.raw, fy.raw, fz.raw, radius.raw,
                                  qx.raw, qy.raw, qz.raw, qw.raw,
                                  interval.raw, static_cast<int64_t>(uncertainty) };
        for (int64_t w : words) h = fixed_mix(h, w);
        return h;
    }

    /*
     * THE ROW INVARIANTS AS ONE PREDICATE, with the slack the arithmetic
     * needs: E and the reach non-negative, |O| <= 1, row 3 a unit quaternion.
     * The slack is a few bits of Q32.32 (2^-16): a truncating normalisation
     * can put |O| or |q| over 1 by the last bits of a divide, and a predicate
     * without the slack would fail on arithmetic the operations are meant to
     * do. Checked by the tester over fuzzed operation sequences, and by any
     * caller that wants to assert a state rather than trust the comment.
     */
    static constexpr int64_t kSlackRaw = int64_t(1) << 16;
    bool Holds() const
    {
        if (energy.raw < 0 || radius.raw < 0) return false;
        const Fixed o2 = ox * ox + oy * oy + oz * oz;
        if (o2.raw > Fixed::One().raw + kSlackRaw) return false;
        const Fixed q2 = qw * qw + qx * qx + qy * qy + qz * qz;
        if (q2.raw > Fixed::One().raw + kSlackRaw || q2.raw < Fixed::One().raw - kSlackRaw) return false;
        return true;
    }

private:
    // The half-angle spinor of an axis and an angle: false when there is no
    // turn to make (zero axis or zero angle).
    static bool halfTurn(Fixed ax, Fixed ay, Fixed az, Fixed angle, Fixed& w, Fixed& X, Fixed& Y, Fixed& Z)
    {
        const Fixed m = Fixed::Length(ax, ay, az);
        if (!m.IsPositive() || angle.IsZero()) return false;
        Fixed sh, ch;
        (angle * Fixed::Half()).SinCos(sh, ch);
        w = ch; X = (ax / m) * sh; Y = (ay / m) * sh; Z = (az / m) * sh;
        return true;
    }
    // Keep row 3 a unit quaternion through repeated composition: the
    // truncation of each product drifts the norm by a few last bits, and a
    // spinor off the unit sphere scales what it rotates.
    void renormalise()
    {
        const Fixed n = (qw * qw + qx * qx + qy * qy + qz * qz).Sqrt();
        if (!n.IsPositive()) { qx = qy = qz = Fixed::Zero(); qw = Fixed::One(); return; }
        qw /= n; qx /= n; qy /= n; qz /= n;
    }
    // O is K scaled by 1/E. E of zero has no direction to record.
    void setKinetic(Fixed kx, Fixed ky, Fixed kz)
    {
        if (!energy.IsPositive()) { Rest(); return; }
        ox = kx / energy; oy = ky / energy; oz = kz / energy;
    }

    // The readings with |O| already taken: the roots are the cost of this
    // struct, and a reading that needs two of its own answers takes each
    // once.
    void directionGiven(Fixed m, Fixed& dx, Fixed& dy, Fixed& dz) const
    {
        if (!m.IsPositive()) { dx = dy = dz = Fixed::Zero(); return; }
        dx = ox / m; dy = oy / m; dz = oz / m;
    }
    Fixed emitGiven(Fixed h, Fixed joules)
    {
        if (!joules.IsPositive()) return Fixed::Zero();
        const Fixed q = Fixed::Min(joules, h);
        if (!q.IsPositive()) return Fixed::Zero();
        const Fixed kx = ox * energy, ky = oy * energy, kz = oz * energy;
        energy -= q;
        setKinetic(kx, ky, kz);
        return q;
    }
    OrderVector emitEventGiven(Fixed h, Fixed joules, Fixed span)
    {
        OrderVector e;
        e.x = x; e.y = y; e.z = z;
        e.id       = id;
        e.interval = span;
        e.energy   = emitGiven(h, joules);
        e.ox = e.oy = e.oz = Fixed::Zero();
        if (!e.energy.IsPositive()) return e;
        e.uncertainty = derive_uncertainty(e);
        return e;
    }
};

#endif
