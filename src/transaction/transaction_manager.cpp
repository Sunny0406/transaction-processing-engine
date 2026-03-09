#include "transaction_manager.h" 
#include <stdexcept> // for std::runtime_error
#include <rocksdb/db.h> // for RocksDB DB and Status
#include <rocksdb/write_batch.h> // for RocksDB WriteBatch
#include <type_traits> // for std::is_pointer_v used when assigning opened DB to member

// WriteBatch allows us to group multiple writes into a single atomic operation at commit time
// Constructor: open (or create) the RocksDB database at the given path
// Destructor: close the database (RocksDBWrapper will handle this)
TransactionManager::TransactionManager(const std::string& db_path) {
    rocksdb::Options options;
    options.create_if_missing = true; // create DB if it doesn't exist yet

    rocksdb::Status status = rocksdb::DB::Open(options, db_path, &db_);
    if (!status.ok()) {
        throw std::runtime_error("Failed to open RocksDB: " + status.ToString());
    }
}


// destructor
// for std::unique_ptr<rocksdb::DB> db_;, we don't need to manually delete db_ 
// since unique_ptr will automatically clean up when TransactionManager is destroyed. 
// However, if we were using a raw pointer, we would need to call delete db_ here to avoid memory leaks. 
// In this implementation, we can rely on unique_ptr's destructor to handle cleanup, so we don't need to explicitly delete db_ in the destructor.
TransactionManager::~TransactionManager() {
    // db_ is a unique_ptr, so it will be automatically destroyed when TransactionManager is destroyed
}

// lifecycle
// Begin: create a new Transaction with a unique ID and store it in txn_map
Transaction* TransactionManager::Begin(){
    // generate a unique transaction ID
    auto txn_id = next_txn_id_.fetch_add(1);
    auto txn = std::make_unique<Transaction>(txn_id);
    auto* ptr = txn.get();
    txn_map_.emplace(txn_id, std::move(txn));
    return ptr;
}

//Commit: flush buffered writes to RocksDB atomically using WriteBatch
bool TransactionManager::Commit(Transaction* txn) {
    // check state and return false if not running
    if (txn->GetStatus() != TxnStatus::RUNNING) {
        return false;
    }

    rocksdb::WriteBatch batch;
    for (const auto& [key, value]: txn->GetWriteBuffer()){
        batch.Put(key, value.dump()); // convert json to string for storage
        // dump method: return a string representation of the JSON value, which can be stored in RocksDB
    }

    rocksdb::WriteOptions write_opts;
    rocksdb::Status status = db_->Write(write_opts, &batch);
    if (!status.ok()) {
        txn->SetStatus(TxnStatus::ABORTED); // mark transaction as aborted on failure
        return false;
    }
    txn->SetStatus(TxnStatus::COMMITTED); // mark transaction as committed on success
    return true;
}

// Abort: mark transaction as aborted and remove it from txn_map
void TransactionManager::Abort(Transaction* txn) {
    // discard write buffer, so nothing writed to DB
    txn->SetStatus(TxnStatus::ABORTED);
}

// Transaction Read/Write

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