#include "../../include/workload_runner.h"

#include <thread>
#include <chrono>
#include <random>
#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// WorkloadStats
// ─────────────────────────────────────────────────────────────────────────────

void WorkloadStats::merge(const WorkloadStats& other) {
    committed        += other.committed;
    aborted          += other.aborted;
    lock_retries     += other.lock_retries;
    total_latency_ms += other.total_latency_ms;
}

void WorkloadStats::print(double elapsed_sec, int num_threads) const {
    int total = committed + aborted;
    std::cout << "\n=== RunWorkload Results ===\n";
    std::cout << "  Threads      : " << num_threads << "\n";
    std::cout << "  Transactions : " << total << "\n";
    std::cout << "  Committed    : " << committed << "\n";
    std::cout << "  Aborted      : " << aborted
              << "  (" << (total > 0 ? 100.0 * aborted / total : 0.0)
              << "%)\n";
    std::cout << "  Lock retries : " << lock_retries << "\n";
    std::cout << "  Elapsed      : " << elapsed_sec << "s\n";
    if (elapsed_sec > 0)
        std::cout << "  Throughput   : "
                  << (committed / elapsed_sec) << " txns/sec\n";
    if (committed > 0)
        std::cout << "  Avg latency  : "
                  << (total_latency_ms / committed) << " ms\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// workerOCC
//
// Runs txns_per_thread transactions under OCC.
// On Commit() failure (validation conflict): count abort, backoff, retry.
// ─────────────────────────────────────────────────────────────────────────────
/*
void workerOCC(
    TransactionManager&             mgr,
    const std::vector<TxnTemplate>& templates,
    const KeySpace&                 ks,
    int                             txns_per_thread,
    double                          hot_prob,
    int                             hot_size,
    int                             thread_id,
    WorkloadStats&                  stats)
{
    // Each thread gets its own rng — sharing one rng across threads is a data race
    std::mt19937 rng(42 + thread_id);

    for (int i = 0; i < txns_per_thread; i++) {
        const TxnTemplate& tmpl = templates[i % templates.size()];

        int local_retries = 0;

        // Retry loop: keep retrying until the transaction commits
        while (true) {
            // debug point
            // std::cout << "Debug attempt" << local_retries << "\n";

            // Pick fresh random keys for each attempt
            auto binding = instantiateTransaction(tmpl, ks, rng, hot_prob, hot_size);

            auto t0 = std::chrono::steady_clock::now();
            try{
                // executeTxn internally calls Begin(), executes the txn body, then calls Commit() which does OCC validation
                // Returns true if committed, false if aborted due to OCC validation failure
                bool committed = executeTxn(tmpl, binding, mgr);

                auto t1 = std::chrono::steady_clock::now();

                if (committed) {
                    stats.committed++;
                    stats.total_latency_ms +=
                        std::chrono::duration<double, std::milli>(t1 - t0).count();
                    break; // transaction done
                } else {
                    // Validation conflict — abort and retry
                    stats.aborted++;
                    local_retries++;
                    // Exponential backoff with jitter to reduce thundering herd
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(LockManager::BackoffMs(local_retries)));
                }
            } catch (const std::exception& e) {
                // Key not found or other fatal error — don't retry, just count and move on
                stats.aborted++;
                std::cerr << "[workerOCC] fatal error, skipping txn: " << e.what() << "\n";
                break;
            }
            
        }
    }
}
*/

// ── Temporarily replace your workerOCC body with this debug version ──────────
// This will tell us EXACTLY where the failure is happening.

void workerOCC(
    TransactionManager&             mgr,
    const std::vector<TxnTemplate>& templates,
    const KeySpace&                 ks,
    int                             txns_per_thread,
    double                          hot_prob,
    int                             hot_size,
    int                             thread_id,
    WorkloadStats&                  stats)
{
    std::mt19937 rng(42 + thread_id);

    for (int i = 0; i < txns_per_thread; i++) {
        const TxnTemplate& tmpl = templates[i % templates.size()];
        int local_retries = 0;

        while (true) {
            // std::cout << "[DBG] txn " << i
            //           << " attempt " << local_retries << "\n";

            // ── Step 1: instantiate binding ───────────────────────────────
            std::map<std::string, std::string> binding;
            try {
                binding = instantiateTransaction(tmpl, ks, rng, hot_prob, hot_size);
                // std::cout << "[DBG]   binding ok: ";
                // for (auto& [p, k] : binding)
                //     std::cout << p << "=" << k << " ";
                // std::cout << "\n";
            } catch (const std::exception& e) {
                // std::cout << "[DBG]   instantiate FAILED: " << e.what() << "\n";
                stats.aborted++;
                break; // ← fatal, skip this txn
            }

            auto t0 = std::chrono::steady_clock::now();

            try {
                // ── Step 2: execute + commit ──────────────────────────────
                // std::cout << "[DBG]   calling executeTxn...\n";
                bool committed = executeTxn(tmpl, binding, mgr);
                // std::cout << "[DBG]   executeTxn returned: "
                //           << (committed ? "COMMITTED" : "ABORTED(OCC)") << "\n";

                auto t1 = std::chrono::steady_clock::now();

                if (committed) {
                    stats.committed++;
                    stats.total_latency_ms +=
                        std::chrono::duration<double, std::milli>(t1 - t0).count();
                    break;
                } else {
                    stats.aborted++;
                    local_retries++;
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(LockManager::BackoffMs(local_retries)));
                }

            } catch (const std::exception& e) {
                // std::cout << "[DBG]   executeTxn THREW: " << e.what() << "\n";
                stats.aborted++;
                break; // ← fatal, skip this txn
            }

            // Safety valve: if retried too many times, something is broken
            // if (local_retries > 10) {
            //     std::cout << "[DBG]   GIVING UP after 10 retries\n";
            //     stats.aborted++;
            //     break;
            // }
        }

        // Only run 1 transaction for debug purposes
        //if (i >= 0) break; // ← remove this line after debugging
    }
}



// ─────────────────────────────────────────────────────────────────────────────
// workerTwoPL
//
// Runs txns_per_thread transactions under Conservative 2PL.
// Acquire ALL locks before executing (all-or-nothing).
// If any lock unavailable: release all, backoff, retry lock acquisition.
// Locks are released inside Commit() / Abort() in TransactionManager.
// ─────────────────────────────────────────────────────────────────────────────
void workerTwoPL(
    TransactionManager&             mgr,
    const std::vector<TxnTemplate>& templates,
    const KeySpace&                 ks,
    int                             txns_per_thread,
    double                          hot_prob,
    int                             hot_size,
    int                             thread_id,
    WorkloadStats&                  stats)
{
    std::mt19937 rng(42 + thread_id);

    for (int i = 0; i < txns_per_thread; i++) {
        const TxnTemplate& tmpl = templates[i % templates.size()];

        // Pick keys for this transaction instance
        auto binding = instantiateTransaction(tmpl, ks, rng, hot_prob, hot_size);

        // Collect all keys this transaction will touch
        std::vector<std::string> keys;
        for (auto& [param, key] : binding) {
            keys.push_back(key);
        }

        // Begin transaction before lock acquisition
        // (txn ID needed by LockManager to track ownership)
        Transaction* txn = mgr.Begin();

        // Lock acquisition loop — all-or-nothing
        // If any lock is held by another txn: release all and retry with backoff
        int lock_retry = 0;
        while (!mgr.AcquireAllLocks(txn, keys)) {
            stats.lock_retries++;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(LockManager::BackoffMs(lock_retry++)));
        }
        // All locks acquired — safe to execute

        auto t0 = std::chrono::steady_clock::now();

        // Execute body with the already-begun txn (locks held throughout)
        // Commit() flushes writes to DB then releases all locks
        bool committed = executeTxnWithTxn(tmpl, binding, mgr, txn);

        auto t1 = std::chrono::steady_clock::now();

        if (committed) {
            stats.committed++;
            stats.total_latency_ms +=
                std::chrono::duration<double, std::milli>(t1 - t0).count();
        } else {
            stats.aborted++;
            // Abort() also releases locks inside TransactionManager
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// RunWorkload
//
// Spawns num_threads workers, joins them, merges stats, prints results.
// ─────────────────────────────────────────────────────────────────────────────
WorkloadStats RunWorkload(
    TransactionManager&             mgr,
    const std::vector<TxnTemplate>& templates,
    const KeySpace&                 ks,
    int                             num_transactions,
    int                             num_threads,
    double                          hot_prob,
    int                             hot_size)
{
    if (templates.empty()) {
        std::cerr << "[RunWorkload] No transaction templates!\n";
        return WorkloadStats{};
    }

    // Distribute transactions evenly across threads
    int txns_per_thread = num_transactions / num_threads;

    std::vector<std::thread>    threads;
    std::vector<WorkloadStats>  per_thread_stats(num_threads); // BUG FIX: was nums_threads

    auto wall_start = std::chrono::steady_clock::now();

    for (int t = 0; t < num_threads; t++) {

        if (mgr.GetCCMode() == CCMode::OCC) {
            threads.emplace_back(workerOCC,
                std::ref(mgr), std::cref(templates), std::cref(ks),
                txns_per_thread, hot_prob, hot_size,
                t, std::ref(per_thread_stats[t]));

        } else if (mgr.GetCCMode() == CCMode::TWO_PL) {
            threads.emplace_back(workerTwoPL,
                std::ref(mgr), std::cref(templates), std::cref(ks),
                txns_per_thread, hot_prob, hot_size,
                t, std::ref(per_thread_stats[t]));

        } else {
            // CCMode::NONE — no CC, run directly without locking/validation
            // BUG FIX: lambda now captures txns_per_thread correctly
            threads.emplace_back([&mgr, &templates, &ks,
                                   txns_per_thread, hot_prob, hot_size,
                                   t, &per_thread_stats]() {
                std::mt19937 rng(42 + t);
                for (int i = 0; i < txns_per_thread; i++) {
                    const TxnTemplate& tmpl = templates[i % templates.size()];
                    auto binding = instantiateTransaction(tmpl, ks, rng, hot_prob, hot_size);
                    executeTxn(tmpl, binding, mgr);
                    per_thread_stats[t].committed++;
                }
            });
        }
    }

    // Wait for all threads to complete
    for (auto& th : threads) th.join();

    auto wall_end = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(wall_end - wall_start).count();

    // Merge per-thread stats into one total
    WorkloadStats total;
    for (auto& s : per_thread_stats) total.merge(s);

    total.print(elapsed, num_threads);
    return total;
}