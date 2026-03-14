#pragma once

#include "transaction.h"
#include "lock_manager.h"

#include <unordered_map>
#include <mutex> // for thread safety in transaction management
#include <memory> 
#include <atomic> // for generating unique transaction IDs
#include <string>

#include <rocksdb/db.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * TransactionManager is responsible for:
 * Managing the lifecycle of transactions,
 * Begin() -> allocate a new transaction
 * Read() -> read from db or transaction's write buffer
 * Write() -> buffer writes in the transaction, not yet visible to others
 * Commit() -> validate and flush writes to db atomically via WriteBatch
 * Abort() -> discard buffered writes, mark transaction as aborted
 */


// --- Concurrency Control mode
enum class CCMode {
    NONE, // single-threaded execution, no concurrency control
    OCC,   // optimistic concurrency control (validate at commit)
    TWO_PL // two-phase locking
};


// -------------------------------------------------------------------------------
// TransactionManager
// Controls the full lifecycle of transactions and enforces the chosen
// concurrency control protocal via cc_mode_.
//
// NONE -> Begin/Read/Write/Commit/Abort with no validation or locking (single thread)
// OCC -> validate read_set at Commit time; abort + retry on conflict
// TWO_PL ->  AccquireAllLocks before execution; ReleaseAll() after Commit/Abort
 
class TransactionManager {
public:

    // add mutex
    mutable std:: mutex txn_map_mutex_; // protects txn_map_ for thread safety

    /**
     * Open (or create) the RocksDB database at the given path.
     * Throws std::runtime_error if the database cannot be opened.
     */

    explicit TransactionManager (rocksdb::DB* db, CCMode mode);
    ~TransactionManager();

    // non-copyable
    // we don't want to accidentally copy the TransactionManager since 
    // it manages a DB instance and active transactions
    TransactionManager(const TransactionManager&) = delete;
    TransactionManager& operator=(const TransactionManager&) = delete;

    // Transaction lifecycle
    Transaction* Begin();
    bool Commit(Transaction* txn);
    void Abort(Transaction* txn);

    //-------------------------------- Data Operations(called inside executeTxn) -----------------------------------------------
    /**
     * Read a key inside a transaction.
     * 1. check write_buffer first (read your own writes)
     * 2. if not in buffer, read from RocksDB and record in read_set
     * Returns false if key not found.
     */
    bool Read(Transaction* txn, const std::string& key, json& value_out);
    /**
     * Buffer a write inside a transaction.
     * Does not touch the ROcksDB until Commit().
     */
    void Write(Transaction* txn, const std::string& key, const json& value);

    // -------2PL only: must be called BEFORE executeTxn body runs
    // Returns true -> all locks acquired, safe to proceed
    // Returns false -> a lock was unavailable, caller must backoff + retry
    bool AcquireAllLocks(Transaction* txn, const std::vector<std::string>& keys);

    // -------Accessors-------
    CCMode GetCCMode() const { return cc_mode_; }
    void SetCCMode(CCMode mode) { cc_mode_ = mode; }

    // helpers
    rocksdb::DB* GetDB() const { return db_; } // for direct DB access if needed (e.g. for building keyspace index)
    
    // direct db access
    bool DirectInsert(const std::string& key, const json& value);
    bool DirectRead(const std::string& key, json& value_out);

private:
    

    // ----- OCC helpers -----
    // check read_set against version_store_; returns true if no conflicts, false if any key was modified since read
    bool ValidateReadSet(const Transaction* txn);
    // After successful commit, stamp every written key with commit_ts
    void UpdateVersionStore(const Transaction* txn, uint64_t commit_ts);

    // ----- Members -----
    rocksdb::DB* db_; // borrows only
    CCMode cc_mode_; // concurrency control mode (NONE, OCC, TWO_PL)

    std::atomic<Transaction::txn_id_t> next_txn_id_{1}; // for generating unique txn IDs

    // All live transactions: id -> Transaction
    std::unordered_map<Transaction::txn_id_t, std::unique_ptr<Transaction>> txn_map_; // active transactions indexed by txn_id

    // --- OCC state ---
    // Monotonically increasing logical clock
    // Begin() stamps txn -> read_ts_ = current clock value
    // Commit() stamps commit_ts = ++global_timestamp_
    std::atomic<uint64_t> global_timestamp_{1}; // for OCC timestamping

    // key -> commit_ts of last writer
    // Validation: if version_store_[key] > txn->read_ts_ -> conflict -> abort
    std::unordered_map<std::string, uint64_t> version_store_; // tracks last
    std::mutex version_store_mutex_; // protects version_store_ for thread safety

    // Only one transaction may validate at a time (sequential validation)
    std::mutex validation_mutex_; // protects validation phase in OCC

    // ---- 2PL state is managed by LockManager, so no additional members needed here for TWO_PL mode yet. ----
    LockManager lock_manager_; // manages locks for TWO_PL mode

};


