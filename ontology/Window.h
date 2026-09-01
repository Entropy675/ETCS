#ifndef SUPERTYPE_WINDOW_H__
#define SUPERTYPE_WINDOW_H__


#include "../core_defs.h"
#include <cstdint>

// ---------------------------------------------------------------
// WindowPosition
// ---------------------------------------------------------------

// basic WindowPosition bucket
struct WindowPosition
{
    int32_t x;
    int32_t y;
};

// ---------------------------------------------------------------
// NativeSurfaceHandle -- the ONE deliberate crack in the "windowing
// is platform-opaque" wall, and only wide enough for one thing to
// pass through it: enough raw OS handle for another module to build
// its own platform-specific surface (e.g. a Vulkan VkSurfaceKHR)
// without that module ever touching GLFW itself.
//
// Concrete window impls (GLFWWindow.h) populate this from INSIDE
// their own compiled code, against whichever windowing library
// they've already initialized -- so a caller reading it back never
// needs to link that library, and there is exactly one initialized
// copy of it in the process. See RenderProvider's OS/VulkanSurface.h
// for the reason this matters: a second, independently-linked copy
// of a windowing library's own static platform state (X11/Wayland
// connection handles live in globals, not per-instance) would be
// invalid the moment anything called into it -- this struct exists
// so nothing but the module that owns the window ever needs that
// library at all.
// ---------------------------------------------------------------

enum class NativeSurfacePlatform : uint8_t { None, X11, Win32 };

struct NativeSurfaceHandle
{
    NativeSurfacePlatform platform = NativeSurfacePlatform::None;
    union
    {
        struct { void* display; unsigned long window; } x11;   // Display*, XID
        struct { void* hinstance; void* hwnd; }        win32;  // HINSTANCE, HWND
    };
};

// ---------------------------------------------------------------
// Window
// ---------------------------------------------------------------
//
// OS window lifecycle ONLY -- create/close/poll, geometry query and
// placement. Input event distribution lives in InputSource.h and
// resize notification in Resizable.h: both used to be bundled in
// here, which meant anything wanting "a Window" generically dragged
// a whole keyboard ring-buffer along whether it wanted input or not,
// and there was no way to have an input source or a resizable
// target that wasn't also a full window. See RenderProvider::Surface,
// which needs Resizable without any of this.

class Window_ : virtual public ETCS::Entity
{
public:
    virtual ~Window_() = default;

    virtual void            CloseWindow()                                           = 0;
    virtual void            CreateWindow(const char* title, uint32_t w, uint32_t h) = 0;
    virtual bool            ShouldClose()                                           = 0;
    virtual void            PollEvents()                                            = 0;
    virtual WindowPosition  GetPosition()                                           = 0;
    virtual void            SetPosition(int32_t x, int32_t y)                       = 0;

    void* GetHandle() { return m_window; }
    bool IsActive() const { return this->hasTag("active"); }

    // Non-dispatched (like GetHandle()) on purpose -- see the struct's own
    // comment. Concrete impls populate m_nativeSurface directly.
    const NativeSurfaceHandle& GetNativeSurfaceHandle() const { return m_nativeSurface; }

protected:
    const char*        m_title  = nullptr;
    void*               m_window = nullptr;
    NativeSurfaceHandle m_nativeSurface{};
};

#endif
