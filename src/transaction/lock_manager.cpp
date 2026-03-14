#include "lock_manager.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
// Internal helper: GetOrCreateEntry
// ─────────────────────────────────────────────────────────────────────────────

LockEntry& LockManager::GetOrCreateEntry(const std::string& key) {
    // table_mutex_ must be held by the caller
    auto it = lock_table_.find(key);
    if (it == lock_table_.end()) {
        auto [inserted_it, _] =
            lock_table_.emplace(key, std::make_unique<LockEntry>());
        return *inserted_it->second;
    }
    return *it->second;
}

// ─────────────────────────────────────────────────────────────────────────────
// AcquireAll
//
// Conservative 2PL: try to take ALL locks at once.
// If any single lock is taken, release everything and return false.
//
// Lock ordering:
//   Keys are sorted lexicographically before acquisition.
//   This ensures all transactions request locks in the same order,
//   which eliminates circular wait and reduces livelock.
// ─────────────────────────────────────────────────────────────────────────────

bool LockManager::AcquireAll(txn_id_t txn_id,
                              const std::vector<std::string>& keys) {
    // De-duplicate and sort for consistent ordering
    std::vector<std::string> sorted_keys(keys.begin(), keys.end());
    std::sort(sorted_keys.begin(), sorted_keys.end());
    sorted_keys.erase(std::unique(sorted_keys.begin(), sorted_keys.end()),
                      sorted_keys.end());

    std::vector<std::string> acquired; // track what we've locked so far

    std::unique_lock<std::mutex> table_lk(table_mutex_);

    for (const auto& key : sorted_keys) {
        LockEntry& entry = GetOrCreateEntry(key);

        if (entry.IsLocked()) {
            // ── Conflict: someone else holds this lock ──────────────────────
            // Release everything we already acquired (all-or-nothing rule)
            for (const auto& held_key : acquired) {
                LockEntry& held_entry = *lock_table_.at(held_key);
                // Lock the individual entry to safely update holder
                std::lock_guard<std::mutex> entry_lk(held_entry.mtx);
                held_entry.holder = INVALID_TXN_ID;
                held_entry.cv.notify_all(); // wake any waiters (multi-thread)
                held_locks_[txn_id].erase(held_key);
            }

            return false; // caller must backoff and retry
        }

        // ── Lock is free: acquire it ────────────────────────────────────────
        {
            std::lock_guard<std::mutex> entry_lk(entry.mtx);
            entry.holder = txn_id;
        }
        acquired.push_back(key);
        held_locks_[txn_id].insert(key);
    }

    // All locks acquired successfully
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReleaseAll
//
// Release every lock held by txn_id.
// Called after Commit() or Abort() — no matter what, locks must be freed.
// ─────────────────────────────────────────────────────────────────────────────

void LockManager::ReleaseAll(txn_id_t txn_id) {
    std::unique_lock<std::mutex> table_lk(table_mutex_);

    auto it = held_locks_.find(txn_id);
    if (it == held_locks_.end()) {
        return; // nothing to release
    }

    for (const auto& key : it->second) {
        auto entry_it = lock_table_.find(key);
        if (entry_it == lock_table_.end()) continue;

        LockEntry& entry = *entry_it->second;
        {
            std::lock_guard<std::mutex> entry_lk(entry.mtx);
            assert(entry.holder == txn_id &&
                   "ReleaseAll: releasing a lock not owned by this txn");
            entry.holder = INVALID_TXN_ID;
        }
        entry.cv.notify_all(); // wake any threads waiting on this key
    }

    held_locks_.erase(it); // clean up tracking
}

// ─────────────────────────────────────────────────────────────────────────────
// Query helpers
// ─────────────────────────────────────────────────────────────────────────────

bool LockManager::IsLocked(const std::string& key) const {
    std::lock_guard<std::mutex> table_lk(table_mutex_);
    auto it = lock_table_.find(key);
    if (it == lock_table_.end()) return false;
    std::lock_guard<std::mutex> entry_lk(it->second->mtx);
    return it->second->IsLocked();
}

bool LockManager::IsLockedBy(const std::string& key, txn_id_t txn_id) const {
    std::lock_guard<std::mutex> table_lk(table_mutex_);
    auto it = lock_table_.find(key);
    if (it == lock_table_.end()) return false;
    std::lock_guard<std::mutex> entry_lk(it->second->mtx);
    return it->second->holder == txn_id;
}

size_t LockManager::LockedKeyCount() const {
    std::lock_guard<std::mutex> table_lk(table_mutex_);
    size_t count = 0;
    for (const auto& [key, entry] : lock_table_) {
        std::lock_guard<std::mutex> entry_lk(entry->mtx);
        if (entry->IsLocked()) ++count;
    }
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// BackoffMs  — livelock prevention helper
//
// Exponential backoff with random jitter:
//   wait = min(base * 2^retry, max) + rand(0, base)
//
// Example progression (base=5, max=200):
//   retry 0:  5  + jitter  ~  5-10ms
//   retry 1:  10 + jitter  ~ 10-15ms
//   retry 2:  20 + jitter  ~ 20-25ms
//   retry 3:  40 + jitter  ~ 40-45ms
//   retry 4:  80 + jitter  ~ 80-85ms
//   retry 5: 160 + jitter  ~160-165ms
//   retry 6: 200 + jitter  ~200-205ms  (capped)
// ─────────────────────────────────────────────────────────────────────────────

int LockManager::BackoffMs(int retry_count, int base_ms, int max_ms) {
    int exp_wait = base_ms;
    for (int i = 0; i < retry_count; ++i) {
        exp_wait *= 2;
        if (exp_wait >= max_ms) { exp_wait = max_ms; break; }
    }

    // thread_local: each thread has its own rng — no mutex, no data race
    thread_local std::mt19937 rng(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::uniform_int_distribution<int> dist(0, base_ms);

    return exp_wait + dist(rng);
}
