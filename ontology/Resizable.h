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
// RenderProvider::Target composes it at the ontology-family level
// (TargetBase.h) because swapchain recreation on resize is not an
// optional per-backend concern the way, say, Deletable is.

class Resizable_ : virtual public ETCS::Entity
{
public:
    virtual ~Resizable_() = default;

    virtual WindowSize GetSize() = 0;

    // Callback fires immediately on registration (with the current size)
    // as well as on every future resize -- callers don't need a separate
    // "get initial size" call before subscribing.
    void OnResize(std::function<void(WindowSize)> callback, int priority = 0)
    {
        m_resizeListeners.insert({priority, callback});
        callback(GetSize());
    }

protected:
    // Fires every registered listener, not just the size update: pulled
    // out during the Window_ split (was `m_size = newSize;` only, so
    // m_resizeListeners was populated on OnResize but never consulted
    // again -- a real bug, not just dead code, since RenderProvider::Target
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
