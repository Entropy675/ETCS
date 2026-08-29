#ifndef SUPERTYPE_DATABASE_H__
#define SUPERTYPE_DATABASE_H__


#include "../core_defs.h"

class Database_ : virtual public ETCS::Entity
{
protected:
    void* db = nullptr;
    ETCS::Buffer dbPath;
    ETCS::Buffer schemaGenerator;
    bool connected = false;
    std::atomic<bool> in_transaction = false;
    
public:
    Database_() {} ;
    virtual ~Database_() = default;
    virtual void CloseConnection() = 0;
    virtual void CreateConnection(const ETCS::Buffer& db) = 0;
    virtual bool InitializeSchema(const ETCS::Buffer& schema) = 0;
    virtual bool ExecuteRaw(ETCS::Buffer& data) = 0;
    
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
