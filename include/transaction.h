#pragma once

#include <string>
#include <unordered_map>
#include <nlohmann/json.hpp>



using json = nlohmann::json;

// transaction class
enum class TxnStatus {
    RUNNING,
    COMMITTED,
    ABORTED
}; 

/*
    Represent a single transaction.
    - read_set: snapshot of values read from the db(key -> json value),
    - write_buffer: buffered writes not yet flushed to db(key -> new json value) 

*/

class Transaction { 
public:
    using txn_id_t = uint64_t; 
    explicit Transaction(txn_id_t id) : txn_id_(id), state_(TxnStatus::RUNNING) {}

    // For OCC: timestamp when this transaction began reading
    

    // Getters and setters
    uint64_t GetReadTs() const { return read_ts_; }
    void SetReadTs(uint64_t ts) { read_ts_ = ts; }

    txn_id_t GetTxnId() const { return txn_id_; }
    TxnStatus GetStatus() const { return state_; }
    void SetStatus(TxnStatus new_status) { state_ = new_status; }

    // for multi threads, we may need to add GetThreadId() to identify which thread 
    // is executing this transaction, but for now we can assume single-threaded execution of transactions

    // write buffer
    void BufferWrite(const std::string& key, const json& value) {
        write_buffer_[key] = value; // add or update buffered write
    }

    bool HasBufferedWrite(const std::string& key) const {
        return write_buffer_.count(key) > 0; // check if key is in write buffer
    }

    const json& GetBufferedWrite(const std::string& key) const {
        return write_buffer_.at(key); // throws if key not in buffer
    } 

    const std::unordered_map<std::string, json>& GetWriteBuffer() const {
        return write_buffer_; // for flushing all writes at commit
    }

    // read set
    void RecordRead(const std::string& key, const json& value) {
        read_set_[key] = value; // record the value read from db
    }
    const std::unordered_map<std::string, json>& GetReadSet() const {
        return read_set_; // for validation in OCC
    }

private:
    uint64_t read_ts_{0};

    txn_id_t txn_id_;
    TxnStatus state_;
    std::unordered_map<std::string, json> read_set_; // read values(for OCC)
    std::unordered_map<std::string, json> write_buffer_; // uncommitted writes
};