#define ETCS_REPL_SHELL
#include "../ETCS.h"

int main()
{
#ifdef ETCS_REPL_SHELL
    repl_shell_enable_vterm();
    repl_shell_load_history();
#endif

    WIRE_CONTEXT();

#ifdef ETCS_REPL_SHELL
    std::thread replThread(repl_shell_loop, std::ref(ctx));
#endif

    ETCS::Entity* window = ETCS::spawn_entity("WindowProvider", "Window", env, loader);
    if (window == nullptr)
    {
        ETCS_LOG("GraphicalLoader", "Could not load WindowProvider:Window!");
        return 0;
    }

    window->call("Window.Create", "600 600 'GraphicalLoader Test GUI'", ctx);

    // ProduceEvents/ConsumeEvents stream pair — producer fires on pool thread,
    // consumer blocks its calling thread. Run both on a dedicated thread so
    // the main thread is free to drive the poll loop.
    std::thread eventStreamThread([window, &ctx]() {
        window->call("Window.ProduceEvents", "Window.ConsumeEvents", "", ctx);
    });

    // Main thread drives the poll loop — PollEvents feeds the producer side
    // of the event stream.
    while (window->hasTag("Active") && !g_sig_int && !g_sig_term)
        window->call("Window.PollEvents", "", ctx);

    window->call("Window.Close", "", ctx);

    // Wait for the event stream to drain and the consumer to exit cleanly.
    if (eventStreamThread.joinable())
        eventStreamThread.join();

#ifdef ETCS_REPL_SHELL
    if (ctx.interrupt) *ctx.interrupt = 1;
    replThread.join();
#endif

    return 0;
}
