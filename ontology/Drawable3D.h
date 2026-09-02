#ifndef SUPERTYPE_DRAWABLE3D_H__
#define SUPERTYPE_DRAWABLE3D_H__


#include "../core_defs.h"
#include "Drawable.h"
#include "Drawable2D.h"
#include <cstdint>

// The camera is named here but not included: it is only ever a pointer in
// this file, and Camera.h includes THIS header for Point3D. The dependency
// runs one way -- a scene knows nothing about cameras except that one can
// be handed to it, while a camera is defined in terms of the scene it
// views. Including Camera.h here would make that a cycle and, worse, would
// say the two families are mutually defined, which they are not.
class Camera_;

// ---------------------------------------------------------------
// Point3D / Box3D / DepthSpan
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

// How far a thing is, from nearest to furthest, along a camera's view.
//
// Scene units, not clip space and not a normalised 0..1: normalised depth
// is a fact about one renderer's projection matrix, and two nodes rendered
// by two different modules could not be compared through it. A distance
// can be compared by anything.
//
// Spelled `nearest`/`furthest` rather than near/far because near and far
// are macros in the Windows headers, and a struct field that only compiles
// on some platforms is not a portable ontology.
struct DepthSpan
{
    float nearest;
    float furthest;
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
// itself supplies it. A camera does. So this family carries the
// constraints the 2D one has no use for: given a camera, produce
// that camera's projected frame, and answer how deep this node is
// in it.
//
// DEPTH LIVES HERE AND ONLY HERE. Depth is what having a third
// dimension MEANS, so the family that has one owns it -- not the
// plane it projects onto, which is flat by definition and would be
// carrying an answer it cannot derive. That asymmetry is the whole
// reason the leaves are separate, and it is load-bearing: a
// Drawable2D with a depth method would be a 3D node that had lost
// its third coordinate, and every compositor would then have to ask
// which of the two answers it was looking at.
//
// It is also what resolves occlusion between parts of a scene with
// no tree relationship at all. Two nodes in different subtrees have
// nothing to compare in the graph; along a camera's view they have
// exactly one thing to compare, and it is this.
//
// AND IT IS WHAT ORDERS A 3D NODE. Orderable sits at the top of this
// lineage (SurfaceBase.h) and requires exactly one comparison
// operator, with the rest derived. For a scene node that operator is
// depth against the camera being drawn -- so DepthFor is not an
// accessory to the ordering, it is the quantity the ordering is on,
// and Reorder after a camera moves is the same cascade as Reorder
// after a hash moves along a seam.
//
// SO A DEPTH BUFFER IS ORDERABLE AT A FINER GRAIN, and saying so is
// the useful part rather than the analogy. Painter's order in 2D
// (Drawable_::collectDrawableChildren, sorted by the leaf's own
// operator<) and a depth buffer in 3D are the SAME relation resolved
// at two different granularities: one answer per node, or one answer
// per sample. What forces the second is that in three dimensions the
// relation is not constant over a node -- two boxes can each be in
// front of the other at different pixels, and three can cycle -- so
// no ordering OF THE NODES reproduces the picture, and the finest
// grain at which "in front of" is still a total order is the pixel.
//
// That is the whole content of the distinction, and it cuts both
// ways: 2D can order by node because a flat node's depth IS constant
// over it, which is why the 2D leaf has no depth method and needs
// none. A Drawable2D_ carrying one would be a 3D node that had lost
// its third coordinate. Orderable claims one answer per node; where
// that claim is true, it is enough, and where it is false, the buffer
// below is what the claim becomes.
//
// THE PROJECTED FRAME IS THE CAMERA, LITERALLY. Project does not
// return a picture of the scene; it fills the camera's own image
// plane, which is an ordinary Drawable2D -- indistinguishable from
// the editor's canvas or a window's root, differing only in what
// fills it. Displaying it is what "showing a camera view" IS.
// Surface.h reserved the slot for this before there was anything to
// put in it; Camera.h is the thing that goes there.
//
// STABLE IDENTITY comes free from that. A frame edge presenting the
// view holds its RID, a UI nests children inside it, a compositor
// blits from it -- all of which break the instant the identity
// moves. It cannot move: the plane is the camera, and the camera
// outlives the frame. Two cameras are two planes, which is how a
// split view is two children rather than a special mode.
//
// EXCLUSIVE WITH Drawable2D. Siblings under Drawable, so a type may
// hold one or the other and never both -- enforced by the compiler,
// see Drawable.h. A node that is both "in the scene" and "on the
// screen" is not a node with two memberships, it is two nodes: the
// 3D one and the camera its projection lands on.

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

    // DEPTH, WHOLE-NODE -- how far this node's nearest and furthest points
    // are along that camera's view, in scene units.
    //
    // Takes the camera rather than reading a stored one because depth is
    // not a property of the node: the same node is near from one view and
    // far from another, and a node holding "its depth" would be holding
    // one camera's answer where every camera needs its own. This is the
    // ordering key (see above), and it is a span rather than a point
    // because two nodes can overlap in depth -- a fact a single number
    // silently discards and every sorted-by-centre renderer then has to
    // discover again as an artefact.
    //
    // A node entirely behind the camera reports a negative span, which is
    // the same "not in this view" answer DepthAt gives per pixel.
    virtual DepthSpan DepthFor(Camera_* camera) = 0;

    // DEPTH, PER PIXEL -- how far the surface visible at that pixel of the
    // camera's frame is, in the same scene units.
    //
    // Coordinates are the camera's own, because the camera IS the frame;
    // there is no third space to convert through. Negative means nothing
    // of this node occupies that pixel -- unambiguous, since a distance
    // along the view direction cannot be, whereas returning the far plane
    // would be indistinguishable from something genuinely sitting on it.
    //
    // This is what lets a second view, a UI element, or another scene's
    // projection be composited INTO the frame at the right depth rather
    // than simply on top of it.
    virtual float DepthAt(Camera_* camera, int32_t x, int32_t y) = 0;

    // THE PROJECTION CONSTRAINT -- the seam between the two leaves, and
    // the only method in this family a 2D node has no analogue for.
    //
    // Fills that camera's image plane with this node's view and returns
    // it as the Drawable2D it already is, so the caller draws, nests or
    // presents it with no knowledge that a scene was ever involved. The
    // return value saves a cross-family resolve; it is not a second
    // plane, and there is never more than one.
    //
    // The frame's SIZE is the camera's size, read off the camera as a
    // surface. That is the payoff of the camera being one: "what would
    // this look like at this size" -- a shadow map, a reflection, a
    // thumbnail -- is answered by making a camera that size, not by
    // passing dimensions alongside a pose and hoping the two agree.
    //
    // nullptr when the camera cannot be realised (a degenerate frustum, a
    // zero-sized frame). A caller that gets one has asked for a view that
    // does not exist, which is a different thing from an empty one.
    virtual Drawable2D_* Project(Camera_* camera) = 0;
};

#endif
