// RenderProviderTesterLoader.cc
//
// End-to-end exercise of the RenderProvider milestone-1 surface against a
// real WindowProvider window: spawn a Window, spawn a RenderProvider
// Instance (VkInstance/device/queue/command pool), attach a Target as a
// CHILD of that window, and drive frames of Clear/DrawRect/Present.
//
// The Target reaches its parent window through the generic interface-
// pointer surface -- getInterfacePointer("Window") for the native X11/Win32
// handle it builds its VkSurfaceKHR from, and ("Resizable") for the size it
// sizes its swapchain to -- so this binary never needs either module's
// internal headers, only the tag/action names below.
//
// Runs headless: Xvfb supplies the X server, Mesa's lavapipe supplies a
// software Vulkan device, so the whole path (surface -> swapchain ->
// render pass -> pipeline -> submit -> present) is exercised for real
// without a GPU or a physical display.
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

    // Child of the window, not a root spawn: Target::Create walks getParent()
    // for the surface handle, so the parent link IS the binding between a
    // target and the window it draws into.
    ETCS::Entity* target = ETCS::make_typed_child("RenderProvider", "Target", window, loader);
    if (!target)
    {
        std::cerr << "Failed to attach RenderProvider:Target to the window\n";
        instance->call("Instance.Delete", "", ctx);
        window->call("Window.Delete", "", ctx);
        return 1;
    }

    const std::string createArgs = std::to_string(instance->getRID()) + " " + shader_dir;
    target->call("Target.Create", createArgs.c_str(), ctx);
    std::cout << "Target RID:" << target->getRID() << " (created with: " << createArgs << ")\n";

    // One explicit frame first -- proves Clear/DrawRect/Present work as
    // individually addressable calls, which is the surface a script (and
    // later PintaProvider) actually uses. RunDemo then drives the rest.
    target->call("Target.Clear", "0.05 0.05 0.08 1.0", ctx);
    target->call("Target.DrawRect", "40 40 200 120 0.85 0.2 0.2 1.0", ctx);
    target->call("Target.Present", "", ctx);
    std::cout << "First frame presented.\n";

    target->call("Target.RunDemo", std::to_string(frames).c_str(), ctx);
    std::cout << "RunDemo(" << frames << ") complete.\n";

    target->call("Target.Delete", "", ctx);
    instance->call("Instance.Delete", "", ctx);
    window->call("Window.Delete", "", ctx);
    ETCS::PendingUnloadRegistry::getInstance().join_all();

    std::cout << "RenderProvider test complete.\n";
    return 0;
}
