#include "../ETCS.h"
int main() 
{
    WIRE_CONTEXT();
    ETCS::Entity* db = ETCS::spawn_entity("DatabaseProvider", "LocalDatabase", env, loader);
    if (!db) {
        ETCS_LOG("TestLoader", "Failed to load DatabaseProvider:LocalDatabase");
        return 1;
    }
    ETCS_LOG("TestLoader", "--- Starting Automated DB Test (Schema Path) ---");

    // 1. Connection + tuning
    db->call("LocalDatabase.Connect", "test.db", ctx);
    db->call("LocalDatabase.ExecuteRaw", "PRAGMA journal_mode=WAL;", ctx);
    db->call("LocalDatabase.ExecuteRaw", "PRAGMA synchronous=NORMAL;", ctx);

    // 2. Schema
    ETCS::Buffer schema("CREATE TABLE IF NOT EXISTS ace_test(id INTEGER PRIMARY KEY, val TEXT);");
    db->call("LocalDatabase.InitializeSchema", schema, ctx);

    // 3. Data population — single transactional call
    db->call("LocalDatabase.ExecuteTransaction", 
        "DELETE FROM ace_test;"
        "INSERT INTO ace_test (val) VALUES ('Test1'), ('Test2'), ('Test3');", 
        ctx);

    // 4. Checkpoint WAL
    db->call("LocalDatabase.ExecuteRaw", "PRAGMA wal_checkpoint(FULL);", ctx);

    // 5. Stream query
    ETCS_LOG("TestLoader", "Running QueryProduce -> RowConsume stream...");
    ETCS::Buffer query("SELECT * FROM ace_test");
    ETCS::Buffer target("LocalDatabase.RowConsume"); 
    db->call("LocalDatabase.QueryProduce", target, query, ctx);
    std::this_thread::sleep_for(std::chrono::milliseconds(500)); 

    // 6. Cleanup
    db->call("LocalDatabase.Disconnect", "", ctx);
    
    ETCS_LOG("TestLoader", "--- Test Complete ---");
    return 0;
}
