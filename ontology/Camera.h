#ifndef SUPERTYPE_CAMERA_H__
#define SUPERTYPE_CAMERA_H__


#include "../core_defs.h"
#include "Drawable2D.h"
#include "Drawable3D.h"
#include "Device.h"
#include <cstdint>
#include <vector>

// ---------------------------------------------------------------
// ViewFrustum
// ---------------------------------------------------------------
//
// The pose and the lens, together, because they are one answer:
// changing either changes what the image is of, and a camera whose
// two halves can be set independently has a window in which it is
// describing a view that does not exist.
//
// Stated in Point3D -- the scene's own vocabulary, from Drawable3D.h
// -- rather than bare float triples, because a pose is a position in
// the space being viewed and nothing else. A camera that measured in
// its own units would need a conversion nobody could write down.
//
// NO FRAME SIZE HERE, deliberately. The camera IS the image plane
// (below), so its size is its size as a surface, read with GetSize.
// Carrying frame dimensions in the view struct would be a second
// copy of an answer the object already has, and the two would drift
// the first time something resized the camera. "What would this look
// like at this size" is answered by making a camera that size.

struct ViewFrustum
{
    // Pose, in the scene's own space.
    Point3D position;
    Point3D look_at;
    Point3D up;

    // Lens.
    float fov_y_radians;
    float near_plane;
    float far_plane;
};

// ---------------------------------------------------------------
// Camera
// ---------------------------------------------------------------
//
// A Drawable2D whose contents are produced by looking at a 3D scene.
//
// A REFINEMENT OF SURFACE, exactly as Drawable is. A camera IS the
// image plane -- not a thing that owns one, not a thing that writes
// into one. Surface.h reserved this slot before there was anything
// to put in it: "the projection of a 3D scene onto a camera's image
// plane at some depth from the origin ... it is not a cousin of the
// editor's canvas, it is literally the same family, differing only
// in what fills it." This is the family that fills it that way.
//
// SPECIFICALLY A Drawable2D, not merely a Surface, and the extra
// step is what makes camera views compose. A projected frame has a
// place in its parent, a shape, a hit test and a DrawInto; being a
// Drawable2D means a camera nests in the 2D tree on exactly the
// terms every other node does. So "the 3D view is the main surface
// and the 2D tree is the UI on top of it" is a composition rather
// than an architecture: the compositor blits a camera without
// knowing it is one, a camera nests inside another camera's frame
// as a picture-in-picture, and none of that needs a case.
//
// It is also why a camera is NOT a Drawable3D. The siblings are
// exclusive by compiler (Drawable.h), and that exclusion says the
// right thing here: the camera is on the screen, the scene is in the
// world, and the projection is the seam between them. A camera that
// held both memberships would be its own subject.
//
// DRIVEN BY A Drawable3D, and that is the direction the causality
// runs. The camera does not walk the scene; it names one and asks it
// to project itself into this camera (Drawable3D.h). The scene owns
// the geometry and the depth that resolves occlusion between parts
// of it with no tree relationship at all; the camera owns the pose,
// the lens, and the pixels that come out. Neither is a component of
// the other, and either can change without the other being rebuilt.
//
// WHICH IS WHY THERE IS NO DEPTH HERE. Depth is a property of having
// three dimensions, so it is exposed on the 3D family's constraint
// surface and asked of the scene, passing this camera (DepthFor,
// DepthAt). A camera is flat -- it cannot derive depth, and a depth
// accessor here would be a cached copy of the scene's answer with
// its own staleness. Ask the thing that has the dimension.
//
// WHAT IS DELIBERATELY NOT HERE, likewise. Projection matrices, clip
// space, culling, anything about HOW the image is produced -- those
// belong to whatever implements Drawable3D, and a camera that named
// them would be describing one renderer. What every camera must
// agree on is only: it has a view, it looks at something, and it is
// a surface you can read.

class Camera_ : virtual public ETCS::Entity
{
public:
    virtual ~Camera_() = default;

    // The pose and the lens, set together -- see ViewFrustum.
    virtual void SetView(ViewFrustum view) = 0;
    virtual ViewFrustum GetView() = 0;

    // What this camera looks at: the RID of a Drawable3D. An RID rather
    // than a pointer because the scene may live in another module, and
    // because a camera outliving its scene should discover that by failing
    // to resolve rather than by dereferencing.
    virtual void SetScene(ETCS::RID scene) = 0;
    virtual ETCS::RID GetScene() = 0;

    // Project the bound scene into this surface, once.
    //
    // The whole of the implementation this family mandates is: resolve the
    // scene, hand it this camera, let it fill the plane. Separate from the
    // setters so a camera can be moved several times and rendered once,
    // and so the frame edge decides when a view is produced rather than
    // every setter doing it implicitly.
    //
    // Returns false when there is nothing to render -- no scene bound, a
    // scene that no longer resolves, a degenerate frustum. A caller that
    // gets false has an empty view for a reason it can log, rather than a
    // stale one it cannot distinguish from a correct one.
    virtual bool Render() = 0;

    /*
 * WHICH WAY THE PROJECTION LANDS -- host pixels, or through a device.
 *
 * A STANDING PREFERENCE, NOT A MODE, and that distinction is the whole design.
 * What a leaf stores is whether it WANTS a device; what anything reads is
 * DeviceProjection() below, which is that preference AND a device still being
 * there. The two can never disagree, because the second is not stored.
 *
 * IT DEFAULTS TO YES, so attaching a Device is the whole of switching. A
 * camera that has one and does not use it is the confusing case -- you added
 * the capability and the picture came out of the CPU anyway -- and nothing was
 * gained by making the caller say so twice.
 *
 * WHICH IS ALSO WHY THIS IS STILL A BOOLEAN. The obvious shape for "use it
 * automatically, but let me override" is three states: auto, forced host,
 * forced device. Written out, auto and forced-device are the SAME function of
 * the graph -- both are "a device if there is one" -- so the third state would
 * have been unobservable, distinguishable only by asking what someone once
 * typed. Two states, one of them the default, says everything the three did.
 *
 * The asymmetry underneath is the same one Device.h states: there is always a
 * CPU, so the host is what a camera falls BACK to, never what it has to be
 * told to leave. Delete the Device child mid-run and the camera quietly draws
 * on the host again -- correct, immediate, and with nothing to reset.
 */
    // Setting this false is the only way to keep a camera on the host once it
    // has a device. Setting it true is a standing preference that takes effect
    // whenever one appears -- it is the default, so scripts rarely say it.
    virtual bool SetDeviceProjection(bool on) = 0;
    virtual bool DeviceProjectionRequested() const = 0;

    /*
 * THE DEVICE THIS CAMERA CAN REACH, or null.
 *
 * Concrete and here rather than dispatched, for the reason Drawable.h gives
 * for hoisting anyChildAnimating: every camera needs the identical walk over
 * the identical child list, and a copy per leaf is how two of them end up
 * disagreeing. The walk is the one the ontology already uses -- typed
 * children, asked by family name, resolved at the point of use rather than
 * cached, since children come and go.
 *
 * FIRST READY ONE WINS. Several Devices under one camera is several devices
 * available to it (Device.h), and picking is a policy no ontology can hold;
 * what this answers is only "is there one", which is the question the toggle
 * turns on. A leaf wanting to choose among them walks the same children.
 */
    Device_* DeviceSource()
    {
        ::std::vector<::std::pair<ETCS::Buffer, ETCS::RID>> kids;
        this->getTypedChildren(kids);
        for (const auto& entry : kids)
        {
            ETCS::Entity* child = this->getTypedChild(entry.first, entry.second);
            if (!child) continue;
            void* iface = child->getInterfacePointer(ETCS::Buffer("Device"));
            if (!iface) continue;
            Device_* d = static_cast<Device_*>(iface);
            if (d->DeviceReady()) return d;
        }
        return nullptr;
    }

    // Wanted AND still available. This is what a projection branches on, and
    // the only thing that should be: it is a fact about the graph right now.
    bool DeviceProjection() { return DeviceProjectionRequested() && DeviceSource() != nullptr; }
};

#endif
