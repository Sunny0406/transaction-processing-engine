// src/storage/rocksdb_wrapper.h
#pragma once

#include <rocksdb/db.h>
#include <nlohmann/json.hpp>
#include <string>
#include <memory>
#include <vector>
using json = nlohmann::json;

class RocksDBWrapper {
public:
    // Constructor: opens (or creates) the database at db_path
    RocksDBWrapper(const std::string& db_path);

    // Destructor: unique_ptr auto-closes DB
    ~RocksDBWrapper() = default;

    // Write: serialize json → string, store in RocksDB
    void put(const std::string& key, const json& value);

    // Read: fetch string from RocksDB, deserialize → json
    json get(const std::string& key);

    // Delete a key
    void del(const std::string& key);

    // Check existence without fetching full value
    bool exists(const std::string& key);

    // Get all known keys (needed for keyspace indexing)
    std::vector<std::string> getAllKeys();

private:
    std::unique_ptr<rocksdb::DB> db_;  // unique_ptr manages lifetime
};