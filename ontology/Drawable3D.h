#ifndef SUPERTYPE_DRAWABLE3D_H__
#define SUPERTYPE_DRAWABLE3D_H__


#include "../core_defs.h"
#include "Drawable.h"
#include "Drawable2D.h"
#include <cstdint>

// Named, not included: Camera.h includes this header (for Point3D). A scene knows
// only that a camera can be handed to it; a camera is defined by the scene.
class Camera_;

// ---------------------------------------------------------------
// Point3D / Box3D / DepthSpan
// ---------------------------------------------------------------
//
// Float where 2D is integral: 2D addresses PIXELS, on a grid; 3D addresses a
// scene, with none. Projection is where one becomes the other, and the only
// place a rounding rule belongs.

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

// How far a thing is, nearest to furthest, along a camera's view. Scene units,
// not clip space or 0..1 -- normalised depth belongs to one renderer's matrix,
// and two modules' nodes couldn't be compared through it. `nearest`/`furthest`
// because near and far are macros in the Windows headers.
struct DepthSpan
{
    float nearest;
    float furthest;
};

// WHERE A NODE IS DRAWN, when that is not where it is: a position (in the
// parent's space, as the node's own) and a facing (a unit quaternion, w x y
// z), or `on` false for the node's own. Floats: it is a picture's number,
// never a state's -- nothing reads it back, nothing hashes it.
struct ShownPose
{
    bool  on = false;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float qw = 1.0f, qx = 0.0f, qy = 0.0f, qz = 0.0f;
};

// ---------------------------------------------------------------
// Drawable3D
// ---------------------------------------------------------------
//
// A Drawable whose space is three-dimensional, and so can't be drawn until
// something says from WHERE. A Drawable2D knows where its pixels go; this
// doesn't, and only a camera supplies it. So this family carries what the 2D
// one has no use for: given a camera, produce its projected frame, and say
// how deep this node is in it.
//
// DEPTH LIVES HERE AND ONLY HERE: it is what a third dimension means. A 2D node
// with a depth method would be a 3D node that lost a coordinate. Depth is also
// what resolves occlusion between nodes with no tree relation, and it is the
// ORDER of a 3D node: Orderable (SurfaceBase.h) needs one comparison, and here
// it is depth against the camera being drawn -- Reorder after a camera moves is
// the same cascade as after a hash moves.
//
// A DEPTH BUFFER IS ORDERABLE AT A FINER GRAIN. Painter's order in 2D
// (collectDrawableChildren, by operator<) and a depth buffer are one relation at
// two granularities. In 3D "in front of" isn't constant over a node -- two boxes
// can each be in front at different pixels, three can cycle -- so the finest
// grain where it is still a total order is the pixel. 2D orders by node because
// a flat node's depth IS constant over it.
//
// THE PROJECTED FRAME IS THE CAMERA. Project fills the camera's own image plane,
// an ordinary Drawable2D like a canvas or a window root; displaying it is what
// showing a camera view is (Surface.h reserved the slot, Camera.h fills it).
// Its identity is stable for free -- the plane is the camera -- so a frame edge,
// a nested UI or a compositor can hold it. Two cameras, two planes: a split view
// is two children, not a mode.
//
// EXCLUSIVE WITH Drawable2D (Drawable.h). A node both in the scene and on the
// screen is two nodes: the 3D one and the camera it lands on.

class Drawable3D_ : virtual public ETCS::Entity
{
public:
    virtual ~Drawable3D_() = default;

    // Where this node sits in its PARENT's space and how much it occupies --
    // never world space, so moving a container moves its subtree for free.
    virtual Box3D Bounds3D() = 0;

    // Is this point, in this node's own space, inside its actual volume?
    // Bounds3D is the box; this is the occupancy (as ContainsLocal in 2D).
    virtual bool ContainsLocal3D(Point3D p) = 0;

    // DEPTH, WHOLE-NODE: nearest and furthest along that camera's view, in scene
    // units. Takes the camera because depth isn't the node's -- the same node is
    // near from one view and far from another. The ordering key (above); a span,
    // not a point, because two nodes can overlap in depth. Wholly behind the
    // camera: a negative span, DepthAt's "not in this view".
    virtual DepthSpan DepthFor(Camera_* camera) = 0;

    // DEPTH, PER PIXEL: how far the surface visible at that pixel is. Camera
    // coordinates, since the camera IS the frame. Negative: nothing of this node
    // there -- unambiguous, where the far plane would look like something on it.
    // What lets another view, a UI or a second scene composite INTO the frame at
    // the right depth instead of on top.
    virtual float DepthAt(Camera_* camera, int32_t x, int32_t y) = 0;

    // THE PROJECTION -- the seam between the leaves, the one method 2D has no
    // analogue for. Fills the camera's image plane and returns it as the
    // Drawable2D it already is (saving the caller a resolve; never a second
    // plane). The frame's size is the camera's, so "this view at that size" (a
    // shadow map, a thumbnail) is a camera of that size. Null when the camera
    // can't be realised (degenerate frustum, zero size): a view that doesn't
    // exist, not an empty one.
    virtual Drawable2D_* Project(Camera_* camera) = 0;

    // DRAWN HERE, NOT WHERE THE RECORD SAYS -- until told `on` false.
    //
    // A world stepped at a fixed rate is a pose a step, and a picture made
    // more often than that shows some poses twice and jumps between others:
    // fast motion judders. Whatever steps the world knows the poses either
    // side of the moment a frame is for, and can blend them; the blend is
    // where the node is DRAWN, so it goes here rather than into the node's
    // rows, which are the record and stay exactly what the step made them.
    // Its children are drawn about it, as about the node itself.
    virtual void ShowAt(ShownPose pose) = 0;
};

#endif
