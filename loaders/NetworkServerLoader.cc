#include "../ETCS.h"

int main()
{
    WIRE_CONTEXT();


    ETCS::Entity* http = ETCS::spawn_entity("NetworkProvider", "HTTPParser", env, loader);
    if (http == nullptr) return 0;

    ETCS_LOG("NetworkServerLoader", "Loaded NetworkProvider:HTTPParser... ");

    // blocking — owns the server lifetime on main thread
    // ctx termination is the shutdown signal
    http->call("HTTPParser.Listen", "8888", ctx);

    return 0;
}
