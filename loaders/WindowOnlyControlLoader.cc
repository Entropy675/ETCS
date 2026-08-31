// Control: WindowProvider only, same lifecycle shape as
// RenderProviderTesterLoader minus RenderProvider. Used to tell a
// RenderProvider-specific teardown bug from a pre-existing one.
#include "../ETCS.h"
#include <iostream>

int main()
{
    WIRE_CONTEXT();
    ETCS::Entity* window = ETCS::spawn_entity("WindowProvider", "Window", env, loader);
    if (!window) { std::cerr << "no window\n"; return 1; }
    window->call("Window.Create", "800 600 Control", ctx);
    window->call("Window.PollEvents", "", ctx);
    window->call("Window.Delete", "", ctx);
    ETCS::PendingUnloadRegistry::getInstance().join_all();
    std::cout << "control complete.\n";
    return 0;
}
