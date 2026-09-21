#ifndef SUPERTYPE_DATABASE_H__
#define SUPERTYPE_DATABASE_H__


#include "../core_defs.h"
#include <cstdint>
#include <cstddef>

// ONE VALUE, EITHER DIRECTION. What a caller binds into a statement and what a
// row hands back, in the five kinds SQL has. Pointers are borrowed, never
// owned: on the way in they must outlive the Bind call (the leaf copies), on
// the way out they are valid until the next Step or the Finalize -- the same
// rule every engine's column accessors already follow, stated once here.
struct DatabaseValue
{
    enum Kind : uint8_t { Null = 0, Integer, Real, Text, Blob };
    Kind        kind = Null;
    int64_t     i = 0;
    double      d = 0.0;
    const void* p = nullptr;     // Text: bytes, no terminator required; Blob: bytes
    size_t      n = 0;

    static DatabaseValue integer(int64_t v)              { DatabaseValue o; o.kind = Integer; o.i = v; return o; }
    static DatabaseValue real(double v)                  { DatabaseValue o; o.kind = Real;    o.d = v; return o; }
    static DatabaseValue text(const char* s, size_t len) { DatabaseValue o; o.kind = Text;    o.p = s; o.n = len; return o; }
    static DatabaseValue blob(const void* b, size_t len) { DatabaseValue o; o.kind = Blob;    o.p = b; o.n = len; return o; }
};

class Database_ : virtual public ETCS::Entity
{
protected:
    void* db = nullptr;
    ETCS::Buffer dbPath;
    ETCS::Buffer schemaGenerator;
    bool connected = false;
    ::std::atomic<bool> in_transaction = false;
    
public:
    Database_() {} ;
    virtual ~Database_() = default;
    virtual void CloseConnection() = 0;
    virtual void CreateConnection(const ETCS::Buffer& db) = 0;
    virtual bool InitializeSchema(const ETCS::Buffer& schema) = 0;
    virtual bool ExecuteRaw(ETCS::Buffer& data) = 0;

    /*
     * ── a statement with bound values, stepped a row at a time ─────────────
     *
     * ExecuteRaw carries SQL TEXT through an argument Buffer, and that channel
     * is 256 bytes (MAX_TAG_BUFFER_SIZE): enough for any statement a script
     * types, and nowhere near a layer's raster. Another module that has BYTES
     * to keep -- a paint page's layers, a file, anything wider than a line --
     * had no way to hand them over except by reaching for the engine's handle,
     * which means including the provider's vendored header and calling symbols
     * its version script hides. So the family states the one surface such a
     * caller needs: prepare, bind, step, read, finalize. Five verbs, and every
     * engine has them under some spelling; a socket underneath changes none of
     * them, which is the same test the transactions below already passed.
     *
     * NOT A QUERY VERB. RowProduce/QueryProduce stream results out of a
     * database as text and stay the right shape for bulk and for mirroring;
     * this is for a caller that holds a Database_* and wants values, not
     * frames. The handle is opaque on purpose -- an engine's statement type is
     * the engine's -- and Step answers in three states because "no more rows"
     * and "it failed" are different facts a caller acts on differently.
     */
    virtual void* Prepare(const char* sql) = 0;                             // nullptr on failure, logged
    virtual bool  Bind(void* stmt, int index, const DatabaseValue& v) = 0;  // index is 1-based, as SQL numbers them
    virtual int   Step(void* stmt) = 0;                                     // 1 a row is ready, 0 done, -1 failed
    virtual bool  Column(void* stmt, int col, DatabaseValue& out) = 0;      // col is 0-based, as results are numbered
    virtual void  Finalize(void* stmt) = 0;

    // Helper for the Type Factory functions to access the handle
    void* GetHandle() { return db; }
    
    ETCS::Buffer ClearSchema() 
    { 
        ETCS::Buffer proxy = schemaGenerator;
        schemaGenerator.clear();
        return proxy;
    }
    
    bool BeginTransaction()
    {
        bool expected = false;
        if (!in_transaction.compare_exchange_strong(expected, true))
            return false; // already in transaction, atomically rejected
        ETCS::Buffer cmd("BEGIN TRANSACTION;");
        if (!ExecuteRaw(cmd))
        {
            in_transaction.store(false); // failed, release
            return false;
        }
        return true;
    }

    bool Commit()
    {
        bool expected = true;
        if (!in_transaction.compare_exchange_strong(expected, false))
            return false; // no active transaction
        ETCS::Buffer cmd("COMMIT;");
        if (!ExecuteRaw(cmd))
        {
            in_transaction.store(true); // failed, restore
            return false;
        }
        return true;
    }

    bool Rollback()
    {
        bool expected = true;
        if (!in_transaction.compare_exchange_strong(expected, false))
            return false;
        ETCS::Buffer cmd("ROLLBACK;");
        if (!ExecuteRaw(cmd))
        {
            in_transaction.store(true);
            return false;
        }
        return true;
    }

    bool IsInTransaction() const { return in_transaction; }
    
    // RAII transaction scope — calls rollback on destruction if not committed
    struct TransactionGuard
    {
        Database_& db;
        bool committed = false;

        TransactionGuard(Database_& db) : db(db) { db.BeginTransaction(); }
        ~TransactionGuard() { if (!committed) db.Rollback(); }
        bool commit() { committed = true; return db.Commit(); }
    };

    TransactionGuard Transaction() { return TransactionGuard(*this); }
    
};

#endif
