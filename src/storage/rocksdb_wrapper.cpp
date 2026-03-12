// src/storage/rocksdb_wrapper.cpp
#include "rocksdb_wrapper.h"

// Constructor
RocksDBWrapper::RocksDBWrapper(const std::string& db_path) {
    rocksdb::Options options;
    options.create_if_missing = true;   // create DB if it doesn't exist yet
    
    rocksdb::Status status = rocksdb::DB::Open(options, db_path, &db_);
    //                                                             ^^^
    //                  &db_ is std::unique_ptr<rocksdb::DB>*
    //                  RocksDB fills it with the opened DB handle
    
    if (!status.ok())
        throw std::runtime_error("Failed to open RocksDB: " + status.ToString());
}

// PUT: json → string → RocksDB
void RocksDBWrapper::put(const std::string& key, const json& value) {
    // value.dump() converts json object to string: {"balance":100,"name":"A"}
    rocksdb::Status s = db_->Put(rocksdb::WriteOptions(), key, value.dump());
    if (!s.ok()) throw std::runtime_error("RocksDB Put failed: " + s.ToString());
}

// GET: RocksDB → string → json
json RocksDBWrapper::get(const std::string& key) {
    std::string raw;  // RocksDB writes the result here
    rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), key, &raw);
    
    if (s.IsNotFound()) throw std::runtime_error("Key not found: " + key);
    if (!s.ok())        throw std::runtime_error("RocksDB Get failed: " + s.ToString());
    
    return json::parse(raw);  // "{"balance":100}" → json object
}

void RocksDBWrapper::del(const std::string& key) {
    rocksdb::Status s = db_->Delete(rocksdb::WriteOptions(), key);
    if (!s.ok()) throw std::runtime_error("RocksDB Delete failed: " + s.ToString());
}

bool RocksDBWrapper::exists(const std::string& key) {
    std::string raw;
    rocksdb::Status s = db_->Get(rocksdb::ReadOptions(), key, &raw);
    return s.ok();  // true if found, false if IsNotFound
}

// Iterate all keys - useful for building keyspace index
std::vector<std::string> RocksDBWrapper::getAllKeys() {
    std::vector<std::string> keys;
    auto* it = db_->NewIterator(rocksdb::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        keys.push_back(it->key().ToString());
    }
    delete it;
    return keys;
}

rocksdb::DB* RocksDBWrapper::getDB() {
    return db_.get();  // return raw pointer from unique_ptr
}
