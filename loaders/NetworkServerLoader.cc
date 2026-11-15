#include "../ETCS.h"
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

// A bare HttpServer on one page, held up by main() until the root signal
// context is interrupted or terminated (ctrl+c / SIGTERM).
//
// HttpServer.Start binds, listens and arms the accept window synchronously
// and returns; it no longer blocks for the server's lifetime the way the old
// HTTPParser.Listen did. So the lifetime is now owned here, explicitly: main
// waits on its own ctx, then deletes the server and waits out the module's
// unload thread. HTTPParser itself is driven BY the ConnectionManager and is
// never called from a loader.
//
//   ./Run_NetworkServerLoader [port]      (default 8888)

int main(int argc, char** argv)
{
    WIRE_CONTEXT();

    int port = 8888;
    if (argc > 1)
    {
        port = std::atoi(argv[1]);
        if (port <= 0 || port > 65535)
        {
            ETCS_LOG("NetworkServerLoader", "port '" << argv[1] << "' is not a port");
            return 1;
        }
    }

    ETCS::Entity* http = ETCS::spawn_entity("NetworkProvider", "HttpServer", env, loader);
    if (http == nullptr)
    {
        ETCS_LOG("NetworkServerLoader", "Failed to load NetworkProvider:HttpServer");
        return 1;
    }

    // Pages attach BEFORE Start (HttpServer.h): Serve walks them in attach order.
    ETCS::Entity* page = ETCS::make_typed_child("NetworkProvider", "StaticHtmlPage", http, loader);
    if (page == nullptr)
    {
        ETCS_LOG("NetworkServerLoader", "Failed to attach StaticHtmlPage");
        http->call("HttpServer.Delete", "", ctx);
        return 1;
    }
    page->call("StaticHtmlPage.SetHtmlRaw",
               "<!DOCTYPE html><html><body><p>ETCS NetworkServerLoader</p></body></html>", ctx);

    const std::string port_str = std::to_string(port);
    http->call("HttpServer.SetPort", port_str.c_str(), ctx);
    http->call("HttpServer.Start", "", ctx);

    ETCS::Buffer started;
    http->call("HttpServer.IsStarted", started, ctx);
    bool live = false;
    started >> live;
    if (!live)
    {
        ETCS_LOG("NetworkServerLoader", "HttpServer did not start on port " << port
                 << " -- see [HttpServer]/[ConnectionManager] above.");
        http->call("HttpServer.Delete", "", ctx);
        ETCS::PendingUnloadRegistry::getInstance().join_all();
        return 1;
    }

    ETCS_LOG("NetworkServerLoader", "Serving on port " << port << " -- ctrl+c to stop.");
    while (!(ctx.isInterrupted() || ctx.isTerminated()))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    ETCS_LOG("NetworkServerLoader", "Signal received, stopping server.");
    http->call("HttpServer.Delete", "", ctx);
    ETCS::PendingUnloadRegistry::getInstance().join_all();   // wait out the module's unload-recheck thread
    return 0;
}
