#ifndef SUPERTYPE_DRAWABLE3D_H__
#define SUPERTYPE_DRAWABLE3D_H__


#include "../core_defs.h"
#include "Drawable.h"
#include "Drawable2D.h"
#include <cstdint>

// ---------------------------------------------------------------
// Point3D / Box3D / CameraSetup
// ---------------------------------------------------------------
//
// Float, where the 2D geometry is integral, and the difference is
// real rather than stylistic: 2D coordinates address PIXELS, which
// are countable and land on a grid, while 3D coordinates address a
// scene, which does not have one. The projection is exactly the step
// where the second becomes the first, and it is the only place a
// rounding rule belongs.

struct Point3D
{
    float x;
    float y;
    float z;
};

// Axis-aligned, in the parent's space, same as Rect2D is.
struct Box3D
{
    Point3D min;
    Point3D max;
};

// Everything needed to turn a 3D node into a 2D one, and nothing else.
//
// frame_width/frame_height are here rather than being read off the
// destination surface because the projection has to be able to answer
// "what would this look like at this size" without one -- a shadow map,
// an offscreen reflection, a thumbnail. The camera says how big its own
// image plane is; where that plane later gets drawn is a separate
// question with a separate answer.
struct CameraSetup
{
    Point3D  position;
    Point3D  look_at;
    Point3D  up;
    float    fov_y_radians;
    float    near_plane;
    float    far_plane;
    uint32_t frame_width;
    uint32_t frame_height;
};

// ---------------------------------------------------------------
// Drawable3D
// ---------------------------------------------------------------
//
// A Drawable whose addressable space is three-dimensional, and which
// therefore cannot be drawn until something says from WHERE.
//
// That is the entire difference between the two leaves, and it is
// why they are siblings rather than one being a special case of the
// other. A Drawable2D already knows where its pixels go; a
// Drawable3D does not, and no amount of information inside the node
// itself supplies it. A camera does. So this family carries one
// constraint the 2D one has no use for: given camera setup
// parameters, produce the Drawable2D that is this node's projected
// frame.
//
// AND IT IS THE SAME SURFACE, LITERALLY. What Project returns is not
// a picture of the scene, it is the camera's image plane -- an
// ordinary Drawable2D, indistinguishable from the editor's canvas or
// a window's root, differing only in what fills it. Displaying it is
// what "showing a camera view" IS. Surface.h's own comment reserved
// the slot for this before there was anything to put in it; this is
// the thing that goes there.
//
// STABLE IDENTITY. Project returns the SAME Drawable2D for the same
// camera every time, not a fresh snapshot per call. It has to: a
// frame edge presenting that surface holds its RID, a UI nests
// children inside it, a compositor blits from it -- all of which
// break the instant the identity moves. The projection UPDATES the
// plane's contents; it does not replace the plane. Two different
// cameras are two different planes, which is exactly right, and is
// how a split view is two children rather than a special mode.
//
// EXCLUSIVE WITH Drawable2D. Siblings under Drawable, so a type may
// hold one or the other and never both -- enforced by the compiler,
// see Drawable.h. A node that is both "in the scene" and "on the
// screen" is not a node with two memberships, it is two nodes: the
// 3D one and the 2D plane its projection lands on.

class Drawable3D_ : virtual public ETCS::Entity
{
public:
    virtual ~Drawable3D_() = default;

    // Where this node sits in its PARENT's space, and how much of it it
    // occupies. Same parent-relative rule as the 2D leaf, one dimension
    // up: a scene node addresses the space its container gave it, never
    // world space, so moving a container moves its subtree with nothing
    // to recompute.
    virtual Box3D Bounds3D() = 0;

    // Is this point, in THIS node's own local space, inside the node's
    // actual volume? The 3D counterpart of ContainsLocal, and the same
    // distinction: Bounds3D gives the enclosing box, this gives the
    // occupancy.
    virtual bool ContainsLocal3D(Point3D p) = 0;

    // THE PROJECTION CONSTRAINT -- the seam between the two leaves, and
    // the only method in this family a 2D node has no analogue for.
    //
    // Returns this node's image plane for that camera: a Drawable2D like
    // any other, which the caller then draws, nests into, or presents
    // with no knowledge that a scene was ever involved. nullptr when the
    // camera cannot be realised (a degenerate frustum, a zero-sized
    // frame) -- a caller that gets one has asked for a view that does not
    // exist, which is a different thing from an empty one.
    virtual Drawable2D_* Project(CameraSetup camera) = 0;
};

#endif
