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
//     row 0   (x,  y,  z,  RID)      where it is, and which thing it is
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
// THE ARROW OF TIME IS STRUCTURAL: |K| never rises except through Impulse.
// Dissipate lowers it, Emit and Advance hold it, Impulse -- work done on the
// point from outside -- is the only operation here that adds to it. Ordered
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
// THE INVARIANT IS STRUCTURAL, not checked. Impulse adds j joules along a
// unit direction: |K| grows by AT MOST j while E grows by exactly j, so
// |O| <= 1 survives every impulse; Dissipate can only lower it. No sequence
// of the operations below leaves a point with more kinetic energy than energy.
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
    // RID in the fourth slot: identity is a coordinate here. Two units at the
    // same position are still two units.
    Fixed     x, y, z;
    ETCS::RID rid = 0;

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
    Fixed KineticEnergy()   const { return KineticFraction() * energy; }
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
        const Fixed ke = m * energy;
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
    OrderVector CommitShare(Fixed share, Fixed span)
    {
        const Fixed h = Heat();
        return emitEventGiven(h, h.IsPositive() ? h * share : Fixed::Zero(), span);
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
    void Absorb(const OrderVector& q)
    {
        const Fixed heat = q.Heat();
        if (heat.IsPositive()) Absorb(heat);
        const Fixed ke = q.KineticEnergy();
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
    bool MayInteractWith(const OrderVector& other) const { return GapTo(other).raw <= 0; }

    // The uncertainty a crossing carries, derived from the crossing itself:
    // identity, quantity and span through a finaliser. A hash, not a
    // generator -- no state to advance. Quantity and span go in as the
    // integers they are, so two crossings that differ at all differ
    // everywhere, and the same on every platform.
    static uint64_t derive_uncertainty(const OrderVector& e)
    {
        uint64_t h = fixed_mix(e.rid, e.energy.raw);
        return fixed_mix(h, e.interval.raw);
    }

    // Every row, as one number: what two runtimes compare to say they have
    // the same history. The meta is included -- it is the step's state.
    uint64_t Hash() const
    {
        uint64_t h = 0x243f6a8885a308d3ull;
        const int64_t words[] = { x.raw, y.raw, z.raw, static_cast<int64_t>(rid),
                                  ox.raw, oy.raw, oz.raw, energy.raw,
                                  fx.raw, fy.raw, fz.raw, radius.raw,
                                  qx.raw, qy.raw, qz.raw, qw.raw,
                                  interval.raw, static_cast<int64_t>(uncertainty) };
        for (int64_t w : words) h = fixed_mix(h, w);
        return h;
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
        e.rid      = rid;
        e.interval = span;
        e.energy   = emitGiven(h, joules);
        e.ox = e.oy = e.oz = Fixed::Zero();
        if (!e.energy.IsPositive()) return e;
        e.uncertainty = derive_uncertainty(e);
        return e;
    }
};

#endif
