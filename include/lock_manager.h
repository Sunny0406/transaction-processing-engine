#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <random>
#include <chrono>
#include <thread>

using txn_id_t = uint64_t;
static constexpr txn_id_t INVALID_TXN_ID = 0;

// ─────────────────────────────────────────────────────────────────────────────
// LockEntry
//
// Represents the lock state for a single key in the lock table.
// Since Conservative 2PL uses exclusive-only locking, only one transaction
// can hold the lock at a time.
// ─────────────────────────────────────────────────────────────────────────────
struct LockEntry {
    /** Which transaction currently holds this lock. INVALID_TXN_ID = unlocked. */
    txn_id_t holder{INVALID_TXN_ID};

    /** Used in multi-threaded mode to wake up waiters after a lock is released. */
    std::condition_variable cv;

    /** Protects this entry's state. */
    std::mutex mtx;

    // Non-copyable because of mutex/cv
    LockEntry()                            = default;
    LockEntry(const LockEntry&)            = delete;
    LockEntry& operator=(const LockEntry&) = delete;

    bool IsLocked() const { return holder != INVALID_TXN_ID; }
};

// ─────────────────────────────────────────────────────────────────────────────
// LockManager
//
// Manages a global lock table for Conservative 2PL.
//
// Protocol:
//   1. Before executing, a transaction calls AcquireAll(txn_id, keys).
//      - Locks are sorted before acquisition (consistent ordering reduces
//        livelock risk).
//      - If ANY key is already locked, ALL acquired locks are released
//        and the call returns false. The caller must wait and retry.
//   2. After commit/abort, the transaction calls ReleaseAll(txn_id).
//
// Livelock prevention:
//   - Consistent lock ordering (sorted keys) reduces circular wait.
//   - Randomized exponential backoff in the retry loop (done by caller
//     using the BackoffMs() helper).
// ─────────────────────────────────────────────────────────────────────────────
class LockManager {
public:
    LockManager()  = default;
    ~LockManager() = default;

    // Non-copyable
    LockManager(const LockManager&)            = delete;
    LockManager& operator=(const LockManager&) = delete;

    // ── Core API ────────────────────────────────────────────────────────────

    /**
     * Try to acquire exclusive locks on ALL given keys for txn_id.
     *
     * Conservative 2PL rule: it's all-or-nothing.
     * If any key is already locked by another transaction, this method
     * releases every lock it just acquired and returns false.
     *
     * @param txn_id   the transaction requesting locks
     * @param keys     all keys the transaction will access
     * @return true    if ALL locks were acquired successfully
     *         false   if any lock was unavailable (caller must backoff + retry)
     */
    bool AcquireAll(txn_id_t txn_id, const std::vector<std::string>& keys);

    /**
     * Release ALL locks currently held by txn_id.
     * Called after Commit() or Abort().
     */
    void ReleaseAll(txn_id_t txn_id);

    /**
     * Check whether a specific key is locked by anyone.
     * Useful for debugging / testing.
     */
    bool IsLocked(const std::string& key) const;

    /**
     * Check whether a specific key is locked by a specific transaction.
     */
    bool IsLockedBy(const std::string& key, txn_id_t txn_id) const;

    /**
     * Returns the number of keys currently locked.
     * Useful for sanity checks / tests.
     */
    size_t LockedKeyCount() const;

    // ── Livelock prevention helper ───────────────────────────────────────────

    /**
     * Returns a randomized backoff duration in milliseconds.
     *
     * Uses exponential backoff with jitter:
     *   base_ms * 2^retry_count + random jitter
     * capped at max_ms.
     *
     * Call this in the retry loop before sleeping:
     *   std::this_thread::sleep_for(
     *       std::chrono::milliseconds(lock_manager.BackoffMs(retry)));
     *
     * @param retry_count  number of retries so far (0-based)
     * @param base_ms      base wait in milliseconds (default 5)
     * @param max_ms       ceiling in milliseconds    (default 200)
     */
    static int BackoffMs(int retry_count, int base_ms = 5, int max_ms = 200);

private:
    /**
     * Get or create the LockEntry for a key.
     * Protected by table_mutex_.
     */
    LockEntry& GetOrCreateEntry(const std::string& key);

    /**
     * Protects the lock_table_ map itself (not individual entries).
     * Individual entries have their own mutex for fine-grained locking.
     */
    mutable std::mutex table_mutex_;

    /** The global lock table: key -> LockEntry */
    std::unordered_map<std::string, std::unique_ptr<LockEntry>> lock_table_;

    /**
     * Tracks which keys each transaction holds.
     * Used by ReleaseAll() to quickly find and release all held locks
     * without scanning the entire lock_table_.
     *
     *   held_locks_[txn_id] = {key1, key2, ...}
     */
    std::unordered_map<txn_id_t, std::unordered_set<std::string>> held_locks_;
};
