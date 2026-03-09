#pragma once

#include "transaction.h"

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

class TransactionManager {
public:
    /**
     * Open (or create) the RocksDB database at the given path.
     * Throws std::runtime_error if the database cannot be opened.
     */

    explicit TransactionManager(const std::string& db_path);
    ~TransactionManager();

    //non-copyable
    // we don't want to accidentally copy the TransactionManager since 
    // it manages a DB instance and active transactions
    TransactionManager(const TransactionManager&) = delete;
    TransactionManager& operator=(const TransactionManager&) = delete;

    // Transaction lifecycle
    Transaction* Begin();
    bool Commit(Transaction* txn);
    void Abort(Transaction* txn);

    // data operations within a transaction
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

    // helpers
    rocksdb::DB* GetDB() const { return db_.get(); } // for direct DB access if needed (e.g. for building keyspace index)
    
private:
    std::unique_ptr<rocksdb::DB> db_; // raw pointer to RocksDB instance (managed by main.cpp)
    std::atomic<Transaction::txn_id_t> next_txn_id_{0}; // for generating unique txn IDs
    std::unordered_map<Transaction::txn_id_t, std::unique_ptr<Transaction>> txn_map_; // active transactions indexed by txn_id
    std::mutex mutex_; // protects active_txns_ for thread safety
};


