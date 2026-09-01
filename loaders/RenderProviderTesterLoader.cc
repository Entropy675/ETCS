// RenderProviderTesterLoader.cc
//
// End-to-end exercise of the RenderProvider surface against a real
// WindowProvider window, in the shape a 2D editor actually uses it:
//
//   Window                        the OS window
//     └── Surface                 [Surface + Presentable + Resizable]
//   ImageSurface  x2              [Surface + Pixels + Resizable] -- layers
//
// Layers are painted CPU-side (Clear/DrawRect on an ImageSurface), one is
// composited onto the other on the CPU (ImageSurface.Blit -- layer onto
// layer), and the result is composited onto the window surface on the GPU
// (Surface.Blit -- upload + textured quad). Same call name at both levels;
// which one you get depends only on which surface you call it on.
//
// Both surfaces are reached through the generic interface-pointer surface
// -- getInterfacePointer("Window")/("Resizable") for the window's native
// handle and size, ("Pixels") for a layer's bytes -- so this binary needs
// neither module's internal headers, only the tag/action names below.
//
// Runs headless: Xvfb supplies the X server, Mesa's lavapipe supplies a
// software Vulkan device, so the whole path (surface -> swapchain ->
// render pass -> upload -> pipeline -> submit -> present) is exercised for
// real without a GPU or a physical display.
//
//   Xvfb :99 -screen 0 1024x768x24 &
//   DISPLAY=:99 ./Run_RenderProviderTesterLoader 60
//
// argv[1] -- frames to render (default 60). Paths are cwd-relative, so run
// it from the ETCS root (that is where modules/RenderProvider/shaders/
// resolves from), same convention run_website.etcs uses for ./www.

#include "../ETCS.h"

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    const uint32_t frames = (argc > 1) ? static_cast<uint32_t>(std::atoi(argv[1])) : 60;
    const std::string shader_dir = (argc > 2) ? argv[2] : "modules/RenderProvider/shaders/";

    WIRE_CONTEXT();

    ETCS::Entity* window = ETCS::spawn_entity("WindowProvider", "Window", env, loader);
    if (!window)
    {
        std::cerr << "Failed to load WindowProvider:Window\n";
        return 1;
    }
    window->call("Window.Create", "800 600 RenderProvider Test", ctx);
    std::cout << "Window RID:" << window->getRID() << "\n";

    ETCS::Entity* instance = ETCS::spawn_entity("RenderProvider", "Instance", env, loader);
    if (!instance)
    {
        std::cerr << "Failed to load RenderProvider:Instance\n";
        window->call("Window.Delete", "", ctx);
        return 1;
    }
    instance->call("Instance.Create", "", ctx);
    std::cout << "Instance RID:" << instance->getRID() << "\n";

    // Child of the window, not a root spawn: Surface::Create walks
    // getParent() for the native handle, so the parent link IS the binding
    // between a surface and the window it draws into.
    ETCS::Entity* surface = ETCS::make_typed_child("RenderProvider", "Surface", window, loader);
    if (!surface)
    {
        std::cerr << "Failed to attach RenderProvider:Surface to the window\n";
        instance->call("Instance.Delete", "", ctx);
        window->call("Window.Delete", "", ctx);
        return 1;
    }

    const std::string createArgs = std::to_string(instance->getRID()) + " " + shader_dir;
    surface->call("Surface.Create", createArgs.c_str(), ctx);
    std::cout << "Surface RID:" << surface->getRID() << " (created with: " << createArgs << ")\n";

    // --- the layer stack -------------------------------------------------
    // Root spawns, not children of anything: a layer has no owner in the
    // scene-graph sense, it is just an addressable region of pixels that
    // whoever is compositing knows the RID of.

    ETCS::Entity* base = ETCS::spawn_entity("RenderProvider", "ImageSurface", env, loader);
    ETCS::Entity* overlay = ETCS::spawn_entity("RenderProvider", "ImageSurface", env, loader);
    if (!base || !overlay)
    {
        std::cerr << "Failed to spawn RenderProvider:ImageSurface layers\n";
        surface->call("Surface.Delete", "", ctx);
        instance->call("Instance.Delete", "", ctx);
        window->call("Window.Delete", "", ctx);
        return 1;
    }

    base->call("ImageSurface.Create", "320 240", ctx);
    base->call("ImageSurface.Clear", "0.15 0.18 0.35 1.0", ctx);
    base->call("ImageSurface.DrawRect", "20 20 120 80 0.9 0.75 0.15 1.0", ctx);
    std::cout << "Base layer RID:" << base->getRID() << " (320x240)\n";

    overlay->call("ImageSurface.Create", "160 120", ctx);
    overlay->call("ImageSurface.Clear", "0.0 0.0 0.0 0.0", ctx);          // fully transparent
    overlay->call("ImageSurface.DrawRect", "0 0 160 60 0.2 0.85 0.4 0.6", ctx);  // half-height, 60% alpha
    std::cout << "Overlay layer RID:" << overlay->getRID() << " (160x120)\n";

    // CPU composite: overlay onto base, at (140, 130), 80% opacity. This is
    // the layer-onto-layer half of the pair -- no GPU involved at all.
    const std::string cpuBlit = std::to_string(overlay->getRID()) + " 140 130 0 0 0.8";
    base->call("ImageSurface.Blit", cpuBlit.c_str(), ctx);
    std::cout << "CPU composite: overlay -> base (" << cpuBlit << ")\n";

    // --- read the composite back through the Pixels seam -----------------
    // This is exactly the access PintaProvider will use: reach a layer's
    // Pixels_ generically, then read/write its bytes directly. Checking
    // three known pixels turns "it did not crash" into "the raster is
    // correct", and the expected values are derived from Pixels_'s own
    // documented source-over arithmetic rather than from a previous run.
    {
        Pixels_* px = static_cast<Pixels_*>(base->getInterfacePointer(ETCS::Buffer("Pixels")));
        if (!px || !px->PixelData())
        {
            std::cerr << "FAIL: base layer exposes no Pixels interface\n";
            return 1;
        }

        auto at = [&](uint32_t x, uint32_t y) -> const uint8_t*
        { return px->PixelData() + (static_cast<size_t>(y) * px->PixelStride()) + (x * 4); };

        auto check = [&](const char* what, uint32_t x, uint32_t y,
                          int r, int g, int b, int a) -> bool
        {
            const uint8_t* p = at(x, y);
            const bool ok = (p[0] == r && p[1] == g && p[2] == b && p[3] == a);
            std::cout << (ok ? "  ok   " : "  FAIL ") << what << " (" << x << "," << y << ") = "
                      << (int)p[0] << "," << (int)p[1] << "," << (int)p[2] << "," << (int)p[3]
                      << (ok ? "" : "  expected ") ;
            if (!ok) std::cout << r << "," << g << "," << b << "," << a;
            std::cout << "\n";
            return ok;
        };

        std::cout << "Pixels readback on base layer (" << px->PixelWidth() << "x" << px->PixelHeight()
                  << ", stride " << px->PixelStride() << "):\n";

        bool allOk = true;
        // Cleared background: ClearTo REPLACES, so exactly the clear colour.
        allOk &= check("cleared background", 200, 20, 38, 46, 89, 255);
        // Opaque DrawRect over it: alpha 1.0 takes the fast path, exact source.
        allOk &= check("opaque rect",         60, 50, 230, 191, 38, 255);
        // Composited overlay: overlay (51,217,102,153) at 0.8 opacity ->
        // effective alpha 122, source-over onto the (38,46,89,255) background.
        allOk &= check("composited overlay", 140, 130, 44, 127, 95, 255);

        if (!allOk)
        {
            std::cerr << "FAIL: CPU composite produced unexpected pixels\n";
            return 1;
        }
        std::cout << "Pixels readback OK -- CPU raster and compositing verified.\n";
    }

    // --- one explicit frame ----------------------------------------------
    // Proves Clear/DrawRect/Blit work as individually addressable calls in
    // call order, which is the surface a script (and later PintaProvider)
    // actually uses. GPU composite: the finished base layer onto the window
    // surface, scaled to 640x480, then a rect ON TOP of it -- if ordering
    // were not preserved the rect would disappear under the blit.
    surface->call("Surface.Clear", "0.05 0.05 0.08 1.0", ctx);
    const std::string gpuBlit = std::to_string(base->getRID()) + " 80 60 640 480 1.0";
    surface->call("Surface.Blit", gpuBlit.c_str(), ctx);
    surface->call("Surface.DrawRect", "40 40 80 40 0.85 0.2 0.2 1.0", ctx);
    surface->call("Surface.Present", "", ctx);
    std::cout << "First frame presented (GPU composite: " << gpuBlit << ")\n";

    surface->call("Surface.RunDemo", std::to_string(frames).c_str(), ctx);
    std::cout << "RunDemo(" << frames << ") complete.\n";

    overlay->call("ImageSurface.Delete", "", ctx);
    base->call("ImageSurface.Delete", "", ctx);
    surface->call("Surface.Delete", "", ctx);
    instance->call("Instance.Delete", "", ctx);
    window->call("Window.Delete", "", ctx);
    ETCS::PendingUnloadRegistry::getInstance().join_all();

    std::cout << "RenderProvider test complete.\n";
    return 0;
}
