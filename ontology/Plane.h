#ifndef ONTOLOGY_PLANE_H__
#define ONTOLOGY_PLANE_H__

#include "../core_defs.h"
#include "Fixed.h"
#include "OrderVector.h"
#include <cstddef>
#include <vector>

// ---------------------------------------------------------------------------
// Plane -- the set of points p with n . p = d, n a unit normal: the one flat
// thing, in the rows' own integers (Fixed.h), so a question asked of a plane
// is answered the same in every runtime, like every other question the rows
// answer.
//
// A PLANE CUTS SPACE IN TWO, and that is the whole of what it is for. Every
// use below is the same question asked of a different thing:
//
//   a point     which side, and how far (Distance, Classify)
//   a sphere    in front, behind, or straddling -- a reach is a sphere, so
//               this is what a plane asks of any OrderVector (ClassifyReach)
//   a set       split into the three (Bisect): the step a partition of the
//               members takes, beside the kd broadphase that finds pairs
//   a ray       where it crosses (Intersect): a pointer's line through a
//               camera meeting a surface, or a plane through a body
//   a motion    the part along the normal and the part across it (Split,
//               Reflect): what a solid contact bounces and what it rubs
//
// A SOLID IS PLANES. A half-space is one plane, its solid side behind it (n
// points out of the solid); a box is six of them about its centre, in its
// own frame (Planes::Box); a convex body is their intersection. A contact
// asks the solid's planes where a sphere is (SolidContact, below), so a
// shape is never a special case of the contact -- only of how many planes it
// has.
//
// The normal is normalised once, when the plane is made (FromPointNormal,
// FromPoints); every reading after assumes it, so a distance is a single dot
// product and not a division.
// ---------------------------------------------------------------------------
struct Plane
{
    Fixed nx, ny = Fixed::One(), nz;   // unit normal: the front side; +y by default
    Fixed d;                           // n . p for every p on the plane

    enum Side : int { Behind = -1, On = 0, InFront = 1, Straddles = 2 };

    // Through a point, facing along a direction (any non-zero length). An
    // invalid plane (a zero normal) has every point On it and crosses nothing.
    static Plane FromPointNormal(Fixed px, Fixed py, Fixed pz, Fixed ax, Fixed ay, Fixed az)
    {
        Plane p;
        const Fixed l = Fixed::Length(ax, ay, az);
        if (!l.IsPositive()) { p.nx = p.ny = p.nz = Fixed::Zero(); p.d = Fixed::Zero(); return p; }
        p.nx = ax / l; p.ny = ay / l; p.nz = az / l;
        p.d  = p.nx * px + p.ny * py + p.nz * pz;
        return p;
    }
    // Through three points, front side by the right hand: a -> b -> c
    // counter-clockwise seen from in front. Collinear points make no plane.
    static Plane FromPoints(Fixed ax, Fixed ay, Fixed az, Fixed bx, Fixed by, Fixed bz,
                            Fixed cx, Fixed cy, Fixed cz)
    {
        const Fixed ux = bx - ax, uy = by - ay, uz = bz - az;
        const Fixed vx = cx - ax, vy = cy - ay, vz = cz - az;
        return FromPointNormal(ax, ay, az, uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx);
    }

    bool Valid() const { return !nx.IsZero() || !ny.IsZero() || !nz.IsZero(); }

    // Signed: positive in front, negative behind.
    Fixed Distance(Fixed px, Fixed py, Fixed pz) const { return nx * px + ny * py + nz * pz - d; }

    // Which side a point is on, `slack` either side counting as on it.
    Side Classify(Fixed px, Fixed py, Fixed pz, Fixed slack = Fixed::Zero()) const
    {
        const Fixed s = Distance(px, py, pz);
        if (s > slack)  return InFront;
        if (s < -slack) return Behind;
        return On;
    }
    // Which side a reach is on: wholly in front, wholly behind, or across.
    // A leaf (radius zero) is a point, and On counts as across: it is in both.
    Side ClassifyReach(const OrderVector& o) const
    {
        const Fixed s = Distance(o.x, o.y, o.z);
        if (s > o.radius)  return InFront;
        if (s < -o.radius) return Behind;
        return Straddles;
    }

    // The nearest point of the plane.
    void Project(Fixed& px, Fixed& py, Fixed& pz) const
    {
        const Fixed s = Distance(px, py, pz);
        px -= nx * s; py -= ny * s; pz -= nz * s;
    }

    // A motion split against the plane: the part along the normal (signed,
    // returned) and the part across it (left in t).
    Fixed Split(Fixed vx, Fixed vy, Fixed vz, Fixed& tx, Fixed& ty, Fixed& tz) const
    {
        const Fixed vn = nx * vx + ny * vy + nz * vz;
        tx = vx - nx * vn; ty = vy - ny * vn; tz = vz - nz * vn;
        return vn;
    }
    // The mirror image of a direction: what a perfect bounce does to it.
    void Reflect(Fixed& vx, Fixed& vy, Fixed& vz) const
    {
        const Fixed two_vn = Fixed::FromInt(2) * (nx * vx + ny * vy + nz * vz);
        vx -= nx * two_vn; vy -= ny * two_vn; vz -= nz * two_vn;
    }

    // Where a ray o + t*v (t >= 0) crosses the plane; false when it runs
    // alongside or the crossing is behind its origin.
    bool Intersect(Fixed ox, Fixed oy, Fixed oz, Fixed vx, Fixed vy, Fixed vz, Fixed& t) const
    {
        const Fixed den = nx * vx + ny * vy + nz * vz;
        if (den.IsZero()) return false;
        t = -Distance(ox, oy, oz) / den;
        return t.raw >= 0;
    }

    Plane Flipped() const { Plane p; p.nx = -nx; p.ny = -ny; p.nz = -nz; p.d = -d; return p; }
    // The same plane carried by a translation (frames here are translations).
    Plane Moved(Fixed dx, Fixed dy, Fixed dz) const { Plane p = *this; p.d += nx * dx + ny * dy + nz * dz; return p; }

    /*
     * BISECT A SET: the indices of the members wholly in front, wholly behind,
     * and across, in the order given. A partition of space is this step taken
     * again on each side; a member across is in both halves' business, which
     * is why it is kept apart rather than assigned -- a reach is a proof about
     * everything inside it (OrderVector::GapTo), and a plane through it proves
     * nothing about either side.
     */
    void Bisect(const OrderVector* members, size_t n, ::std::vector<size_t>& front,
                ::std::vector<size_t>& behind, ::std::vector<size_t>& across) const
    {
        for (size_t i = 0; i < n; ++i)
        {
            switch (ClassifyReach(members[i]))
            {
                case InFront: front.push_back(i);  break;
                case Behind:  behind.push_back(i); break;
                default:      across.push_back(i); break;
            }
        }
    }
};

/*
 * A SPHERE AGAINST A SOLID, asked of the solid's planes: the normal out of
 * the solid at the nearest point, and how deep the sphere is (positive: in
 * contact, by that much; the contact's slack lets a resting sphere count at
 * zero). Everything in the solid's own frame: the caller moves the sphere's
 * centre in, and the normal back out.
 */
struct SolidContact
{
    Fixed nx, ny, nz;   // out of the solid, toward the sphere
    Fixed depth;        // penetration: radius - distance to the solid
    bool  touching = false;
};

namespace Planes
{
    // A box of half extents h about the origin, as the six planes it is: the
    // faces, each facing out. The order is +x, -x, +y, -y, +z, -z.
    inline void Box(Fixed hx, Fixed hy, Fixed hz, Plane out[6])
    {
        const Fixed z = Fixed::Zero(), o = Fixed::One();
        out[0] = Plane::FromPointNormal( hx, z, z,  o, z, z);
        out[1] = Plane::FromPointNormal(-hx, z, z, -o, z, z);
        out[2] = Plane::FromPointNormal( z, hy, z,  z, o, z);
        out[3] = Plane::FromPointNormal( z,-hy, z,  z,-o, z);
        out[4] = Plane::FromPointNormal( z, z, hz,  z, z, o);
        out[5] = Plane::FromPointNormal( z, z,-hz,  z, z,-o);
    }

    // A half-space: solid behind `p`.
    inline SolidContact SphereHalfSpace(const Plane& p, Fixed cx, Fixed cy, Fixed cz, Fixed r, Fixed slack)
    {
        SolidContact c;
        if (!p.Valid()) return c;
        const Fixed s = p.Distance(cx, cy, cz);
        c.depth = r - s;
        c.nx = p.nx; c.ny = p.ny; c.nz = p.nz;
        c.touching = c.depth + slack > Fixed::Zero();
        return c;
    }

    /*
     * A sphere against a box of half extents h about the origin -- the six
     * planes of Box, asked at once. Outside, the nearest point is the centre clamped into the box (the
     * planes the centre is in front of are the ones it is outside of); inside,
     * the sphere is held by the face it is nearest to leaving through, the
     * plane of least depth. An edge or a corner is the clamp landing on two or
     * three planes at once, which is what lets a ball roll over a lip.
     */
    inline SolidContact SphereBox(Fixed hx, Fixed hy, Fixed hz, Fixed cx, Fixed cy, Fixed cz, Fixed r, Fixed slack)
    {
        SolidContact c;
        auto clamp = [](Fixed v, Fixed h) { return v > h ? h : (v < -h ? -h : v); };
        const Fixed qx = clamp(cx, hx), qy = clamp(cy, hy), qz = clamp(cz, hz);
        const Fixed dx = cx - qx, dy = cy - qy, dz = cz - qz;
        if (!dx.IsZero() || !dy.IsZero() || !dz.IsZero())
        {
            const Fixed dist = Fixed::Length(dx, dy, dz);
            if (!dist.IsPositive()) return c;
            c.depth = r - dist;
            if (c.depth + slack <= Fixed::Zero()) return c;
            c.nx = dx / dist; c.ny = dy / dist; c.nz = dz / dist;
            c.touching = true;
            return c;
        }
        // The centre is inside: out through the nearest face.
        const Fixed ex = hx - cx.Abs(), ey = hy - cy.Abs(), ez = hz - cz.Abs();
        c.nx = c.ny = c.nz = Fixed::Zero();
        if (ex <= ey && ex <= ez) { c.nx = cx.raw < 0 ? -Fixed::One() : Fixed::One(); c.depth = r + ex; }
        else if (ey <= ez)        { c.ny = cy.raw < 0 ? -Fixed::One() : Fixed::One(); c.depth = r + ey; }
        else                      { c.nz = cz.raw < 0 ? -Fixed::One() : Fixed::One(); c.depth = r + ez; }
        c.touching = true;
        return c;
    }

    // Two spheres: the normal from b's centre to a's.
    inline SolidContact SphereSphere(Fixed ax, Fixed ay, Fixed az, Fixed ar,
                                     Fixed bx, Fixed by, Fixed bz, Fixed br, Fixed slack)
    {
        SolidContact c;
        const Fixed dx = ax - bx, dy = ay - by, dz = az - bz;
        const Fixed dist = Fixed::Length(dx, dy, dz);
        c.depth = ar + br - dist;
        if (c.depth + slack <= Fixed::Zero() || !dist.IsPositive()) return c;
        c.nx = dx / dist; c.ny = dy / dist; c.nz = dz / dist;
        c.touching = true;
        return c;
    }
}

#endif // ONTOLOGY_PLANE_H__
