#ifndef SUPERTYPE_RESIZABLE_H__
#define SUPERTYPE_RESIZABLE_H__


#include "../core_defs.h"
#include <cstdint>
#include <functional>
#include <map>

// ---------------------------------------------------------------
// WindowSize
// ---------------------------------------------------------------

struct WindowSize
{
    uint32_t width;
    uint32_t height;

    float aspectRatio() const
    {
        if (height == 0) return 1.0f;
        return static_cast<float>(width) / static_cast<float>(height);
    }
};

// ---------------------------------------------------------------
// Resizable
// ---------------------------------------------------------------
//
// Size + resize notification -- split out of what used to be
// Window_ (see Window.h's own comment). Not window-specific: this
// is "a thing with a 2D size that changes and tells listeners" --
// RenderProvider's surfaces compose it at the ontology-family level
// (SurfaceBase.h) because swapchain recreation on resize is not an
// optional per-backend concern the way, say, Deletable is.

class Resizable_ : virtual public ETCS::Entity
{
public:
    virtual ~Resizable_() = default;

    virtual WindowSize GetSize() = 0;

    /*
     * BE this size. The verb half of the family, which until now was all
     * notification: a Resizable could say how big it is and tell you when
     * that changed, and there was no way to ask it to change.
     *
     * That gap is why a resize event reached a window, recreated its
     * swapchain, and left every surface and compositor beneath it at the
     * size they were spawned at. A listener could hear the new size; it
     * could not act on it without knowing the concrete type on the far end.
     *
     * Default false -- "I have a size but it is not mine to set" is a real
     * and common answer (a window's size belongs to the WM, an image's to
     * the file it came from), and it is the honest one for anything that has
     * not opted in. A caller checks the return rather than assuming.
     */
    virtual bool ResizeTo(WindowSize) { return false; }

    // Callback fires immediately on registration (with the current size)
    // as well as on every future resize -- callers don't need a separate
    // "get initial size" call before subscribing.
    void OnResize(std::function<void(WindowSize)> callback, int priority = 0)
    {
        m_resizeListeners.insert({priority, callback});
        callback(GetSize());
    }

    /*
     * Track `source`: whenever it resizes, so does this.
     *
     * The two halves above joined into the thing everyone actually wanted,
     * so the chain from the WM down to a canvas is one line per link rather
     * than a lambda per link written four times.
     *
     * RESOLVED BY RID AT FIRE TIME, NOT CAPTURED AS A POINTER. The listener
     * outlives its registration by definition, and the follower can be
     * deleted while the source is still being dragged -- a captured `this`
     * is then a call into freed memory on the very next mouse move. The hold
     * is the same one every other cross-entity walk takes (Entity.h): falsy
     * means gone or going, and a follower that is going wants no more sizes.
     *
     * Registered on the SOURCE, because that is where the size arrives. The
     * initial fire OnResize does for free is the initial layout.
     */
    void FollowResize(Resizable_* source)
    {
        if (!source || source == this) return;
        const ETCS::RID me = getRID();
        source->OnResize([me](WindowSize s)
        {
            ETCS::Held<Resizable_> f = ETCS::resolve_held<Resizable_>("Resizable", me);
            if (!f) return;
            f->ResizeTo(s);
        });
    }

protected:
    // Fires every registered listener, not just the size update: pulled
    // out during the Window_ split (was `m_size = newSize;` only, so
    // m_resizeListeners was populated on OnResize but never consulted
    // again -- a real bug, not just dead code, since RenderProvider::Surface
    // depends on an ACTUAL resize (not just the initial OnResize call)
    // recreating its swapchain. Fixed here rather than left for whoever
    // first needed post-registration resize delivery to hit it.
    void notifyResize(WindowSize newSize)
    {
        m_size = newSize;
        for (auto& [priority, callback] : m_resizeListeners)
        {
            (void)priority;
            callback(newSize);
        }
    }

    WindowSize m_size = {};

private:
    std::multimap<int, std::function<void(WindowSize)>, std::greater<int>> m_resizeListeners;
};

#endif
