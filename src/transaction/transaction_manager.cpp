#include "transaction_manager.h" 
#include "transaction.h"
#include "lock_manager.h"

#include <stdexcept> // for std::runtime_error
#include <rocksdb/db.h> // for RocksDB DB and Status
#include <rocksdb/write_batch.h> // for RocksDB WriteBatch
#include <type_traits> // for std::is_pointer_v used when assigning opened DB to member
#include <iostream> // for debug printing
// WriteBatch allows us to group multiple writes into a single atomic operation at commit time


// ---------------------------------------------------------------------
// Constructor
// Takes a raw rocksdb::DB* from your existing RocksDBWrapper.
// RocksDBWrapper keeps ownership - TransactionManager just borrows it.
// -------------------------------------------------------------------------=
TransactionManager::TransactionManager(rocksdb::DB* db, CCMode mode) : db_(db), cc_mode_(mode) {
    if (!db_) {
        throw std::runtime_error("TransactionManager: null DB pointer");
    }
}
TransactionManager::~TransactionManager(){}

// destructor
// for std::unique_ptr<rocksdb::DB> db_;, we don't need to manually delete db_ 
// since unique_ptr will automatically clean up when TransactionManager is destroyed. 
// However, if we were using a raw pointer, we would need to call delete db_ here to avoid memory leaks. 
// In this implementation, we can rely on unique_ptr's destructor to handle cleanup, so we don't need to explicitly delete db_ in the destructor.
// TransactionManager::~TransactionManager() {
//     // db_ is a unique_ptr, so it will be automatically destroyed when TransactionManager is destroyed
// }

// lifecycle
// ---------------------------------------------------------------------------------------
// Begin
//
// all modes: create a new Txn and assign it a unique ID.
// OCC only: snapshot the current global timestamp as read_ts_.
//           Any key whose version_store_ entry is > read_ts_ at commit time 
//           means the key was modified after this transaction started → conflict.
// ---------------------------------------------------------------------------------------
Transaction* TransactionManager::Begin(){
    // generate a unique transaction ID
    auto txn_id = next_txn_id_.fetch_add(1);
    auto txn = std::make_unique<Transaction>(txn_id);
    auto* ptr = txn.get(); // keep raw pointer for return, ownership is still with unique_ptr in txn_map

    // OCC: record the timestamp at which this transaction started reading
    if (cc_mode_ == CCMode::OCC) {
        ptr->SetReadTs(global_timestamp_.load()); // read_ts_ = current global timestamp
         //std::cout << "[DEBUG] txn " << txn_id
         //         << " read_ts=" << ptr->GetReadTs() << "\n";  // should NOT be 0
    }

    {
        std::lock_guard<std::mutex> lk(txn_map_mutex_); // lock txn_map for thread safety
        txn_map_.emplace(txn_id, std::move(txn)); // store the transaction in the map, ownership is transferred to txn_map
    }

    return ptr;
}

// ---------------------------------------------------------------------------------------
//Commit
//
// NONE -> flush write_buffer to RocksDB, done.
//
// OCC -> 1. Acquire validation_mutex_ (sequential validation)
//        2. For every key in read_set: check version_store_[key] > read_ts_
//           -> If any conflict found, release validation_mutex_, mark txn aborted, return false
//        3. Assign commit_ts = ++global_timestamp_
//        4. Flush write_buffer to RocksDB via WriteBatch
//        5. Stamp version_store_[key] = commit_ts for each written key
//        6. Release validation_mutex_, mark txn committed, return true
//
// 2PL -> flush write_buffer to RocksDB, then release all locks
//        (Locks were acquired before executeTxn body ran via AcquireAllLocks)
bool TransactionManager::Commit(Transaction* txn) {

    // ── TEMPORARY DEBUG ─────────────────────────────────────────────
    // std::cout << "[COMMIT] txn_id=" << txn->GetTxnId()
    //           << " status=" << static_cast<int>(txn->GetStatus())
    //           << " cc_mode=" << static_cast<int>(cc_mode_)
    //           << " read_set=" << txn->GetReadSet().size()
    //           << " write_buf=" << txn->GetWriteBuffer().size() << "\n";
    // ────────────────────────────────────────────────────────────────


    // check state and return false if not running
    if (txn->GetStatus() != TxnStatus::RUNNING) {
        return false;
    }

    // -- OCC: validate before writing --
    if (cc_mode_ == CCMode::OCC) {
        // Acquire validation lock - only one txn validates at a time
        std::unique_lock<std::mutex> val_lock(validation_mutex_);

        if (!ValidateReadSet(txn)) {
            // Conflict detected: abort this transaction
            // val_lock released automatically on scope exit
            txn->SetStatus(TxnStatus::ABORTED); // mark transaction as aborted on conflict
            return false;
        }

        // Assign a commit timestamp while still holding the validation lock
        uint64_t commit_ts = global_timestamp_.fetch_add(1) + 1; // increment global timestamp for this commit
        
        // Flush writes to RocksDB
        rocksdb::WriteBatch batch;
        for (const auto& [key, value] : txn -> GetWriteBuffer()) {
            batch.Put(key, value.dump()); // convert json to string for storage
            // dump method: return a string representation of the JSON value, which can be stored in RocksDB
        }
        rocksdb::Status status = db_->Write(rocksdb::WriteOptions(), &batch);
        if (!status.ok()) {
            txn->SetStatus(TxnStatus::ABORTED); // mark transaction as aborted on failure
            return false;
        }

        // Swap version_store with commit_ts for each written key
        UpdateVersionStore(txn, commit_ts);

        txn->SetStatus(TxnStatus::COMMITTED); // mark transaction as committed
        return true;
        // val_lock released automatically on scope exit


    }
    // --- NONE / 2PL: flush to RocksDB -------------------------------------------------------------------
    rocksdb::WriteBatch batch;
    for (const auto& [key, value]: txn->GetWriteBuffer()){
        batch.Put(key, value.dump()); // convert json to string for storage
        // dump method: return a string representation of the JSON value, which can be stored in RocksDB
    }

    rocksdb::WriteOptions write_opts;
    rocksdb::Status status = db_->Write(write_opts, &batch);
    if (!status.ok()) {
        txn->SetStatus(TxnStatus::ABORTED); // mark transaction as aborted on failure
        if (cc_mode_ == CCMode::TWO_PL){
            // release locks if 2PL
            lock_manager_.ReleaseAll(txn->GetTxnId());
        }
        return false;
    }
    
    txn->SetStatus(TxnStatus::COMMITTED); // mark transaction as committed on success
    
    // clean the txns in txn_map_
    // {
    //     std::lock_guard<std::mutex> lk(txn_map_mutex_); // lock txn_map for thread safety
    //     txn_map_.erase(txn->GetTxnId()); // remove the transaction from the map since it's no longer active
    // }

    // --- 2PL: release locks after commit -------------------------------------------------------------------
    if (cc_mode_ == CCMode::TWO_PL){
        // release locks if 2PL
        lock_manager_.ReleaseAll(txn->GetTxnId());
    }

    return true;
}


// ---------------------------------------------------------------------------------------
// Abort
//
// ALL modes: discard write_buffer (nothing written to DB).
// 2PL only: release any locks this transaction holds.
// ---------------------------------------------------------------------------------------
void TransactionManager::Abort(Transaction* txn) {
    // discard write buffer, so nothing writed to DB
    txn->SetStatus(TxnStatus::ABORTED);

    // clean the txns in txn_masp_
    // {
    //     std::lock_guard<std::mutex> lk(txn_map_mutex_); // lock txn_map for thread safety
    //     txn_map_.erase(txn->GetTxnId()); // remove the transaction from the map since it's no longer active
    // }

    if (cc_mode_ == CCMode::TWO_PL){
        // release locks if 2PL
        lock_manager_.ReleaseAll(txn->GetTxnId());
    }

}

// Transaction Read/Write

// ---------------------------------------------------------------------------------------
// Read
//
// ALL modes:
// 1. check write_buffer first (read your own writes)
// 2. Fall back to RocksDB
// 3. Record in read_set_ (used by OCC validation at commit time)

bool TransactionManager::Read(Transaction* txn, const std::string&key, json& out_value) {
    // check write buffer first (read your own writes)
    if (txn->GetStatus() != TxnStatus::RUNNING) {
        throw std::runtime_error("Cannot read from a non-running transaction");
    }
    // 1. Read your own writes from the buffer
    if (txn->HasBufferedWrite(key)) {
        out_value = txn->GetBufferedWrite(key);
        // also record in read set for OCC validation later
        txn->RecordRead(key, out_value);
        return true;
    }
    // 2. If not in buffer, read from RocksDB
    std::string raw;
    rocksdb::ReadOptions read_opts;
    rocksdb::Status status = db_->Get(read_opts, key, &raw);
    if (status.IsNotFound()) {
        return false; // key not found
    }
    if (!status.ok()) {
        throw std::runtime_error("Failed to read from RocksDB: " + status.ToString());
    }

    out_value = json::parse(raw); // parse string back to json
    // record in read_set (value seen at this point in time) for OCC validation later
    txn->RecordRead(key, out_value);
    return true;
}

void TransactionManager::Write(Transaction* txn, const std::string& key, const json& value) {
    if (txn->GetStatus() != TxnStatus::RUNNING) {
        throw std::runtime_error("Cannot write to a non-running transaction");
    }
    // buffer the write in the transaction's write buffer
    txn->BufferWrite(key, value);
}

// direct DB access (data loading only)
bool TransactionManager::DirectInsert(const std::string& key, const json& value){
    std::string raw;
    rocksdb::Status status = db_->Put(rocksdb::WriteOptions(), key, value.dump());
    return status.ok();
}

bool TransactionManager::DirectRead(const std::string& key, json& out_value){
    std::string raw;
    rocksdb::Status status = db_->Get(rocksdb::ReadOptions(), key, &raw);
    if(!status.ok()) return false;
    out_value = json::parse(raw);
    return true;
}

// ---------------------------------------------------
// AcquireAllLocks for 2PL
//
// Called by the workload runner BEFORE executeTxn body runs.
//
// Usage in workload runner:
//   int retry = 0;
//   while (!mgr.AcquireLocks(txn, keys)) {
//      std::this_thread::sleep_for{
//          std::chrono::milliseconds(LockManager::BackoffMs(retry++))}
//   }
//   executeTxn(tmpl, binding, mgr);
// ---------------------------------------------------
bool TransactionManager::AcquireAllLocks(Transaction* txn,
                                         const std::vector<std::string>& keys){
    return lock_manager_.AcquireAll(txn->GetTxnId(), keys);
}

// ---------------------------------------------------
// ValidateOCC (private)
//
// For every key the transaction READ:
//   version_store_[key] > txn->read_ts_ -> stale read -> conflict -> abort
//
// Return true = no conflict
// Return false = conflict detected
// ---------------------------------------------------
bool TransactionManager::ValidateReadSet(const Transaction* txn) {
    std::lock_guard<std::mutex> vs_lock(version_store_mutex_);

    uint64_t read_ts = txn->GetReadTs();

    // ── TEMPORARY DEBUG ──────────────────────────────────────────────
    // std::cout << "[VALIDATE] read_ts=" << read_ts
    //           << " read_set_size=" << txn->GetReadSet().size()
    //           << " version_store_size=" << version_store_.size() << "\n";
    // for (const auto& [key, ver] : version_store_) {
    //     std::cout << "[VALIDATE]   version_store[" << key << "]=" << ver << "\n";
    // }
    // ─────────────────────────────────────────────────────────────────

    for (const auto& [key, _] : txn->GetReadSet()) {
        auto it = version_store_.find(key);
        if (it == version_store_.end()) continue;
        if (it->second > read_ts) {
            // std::cout << "[VALIDATE] CONFLICT: key=" << key
            //           << " version=" << it->second
            //           << " > read_ts=" << read_ts << "\n";
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------
// UpdateVersionStoreOCC (private)
//
// Stamp every written key with commit_ts so future OCC validators can detect
// conflicts against this transaction's writes.
// ---------------------------------------------------
void TransactionManager::UpdateVersionStore(const Transaction* txn, uint64_t commit_ts){
    std::lock_guard<std::mutex> vs_lock(version_store_mutex_); // protect version_store_ during update

    for (const auto& [key, _] : txn->GetWriteBuffer()){
        version_store_[key] = commit_ts; // update version store with this transaction's commit timestamp for each written key
    }
}