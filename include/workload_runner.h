#pragma once

#include <vector>
#include <string>
#include <iostream>

#include "transaction_manager.h"
#include "input_parse.h"

// ─────────────────────────────────────────────────────────────────────────────
// WorkloadStats
// Per-thread stats collected during execution, merged at the end.
// ─────────────────────────────────────────────────────────────────────────────
struct WorkloadStats {
    int    committed{0};
    int    aborted{0};           // OCC validation failures
    int    lock_retries{0};      // 2PL lock acquisition retries
    double total_latency_ms{0.0};

    void merge(const WorkloadStats& other);
    void print(double elapsed_sec, int num_threads) const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Worker functions — one per CC protocol.
// ─────────────────────────────────────────────────────────────────────────────

// OCC worker: retry on Commit() failure (validation conflict)
void workerOCC(
    TransactionManager&             mgr,
    const std::vector<TxnTemplate>& templates,
    const KeySpace&                 ks,
    int                             txns_per_thread,
    double                          hot_prob,
    int                             hot_size,
    int                             thread_id,
    WorkloadStats&                  stats);

// 2PL worker: acquire all locks before executing, backoff+retry if unavailable
void workerTwoPL(
    TransactionManager&             mgr,
    const std::vector<TxnTemplate>& templates,
    const KeySpace&                 ks,
    int                             txns_per_thread,
    double                          hot_prob,
    int                             hot_size,
    int                             thread_id,
    WorkloadStats&                  stats);

// ─────────────────────────────────────────────────────────────────────────────
// RunWorkload — main entry point
// ─────────────────────────────────────────────────────────────────────────────
WorkloadStats RunWorkload(
    TransactionManager&             mgr,
    const std::vector<TxnTemplate>& templates,
    const KeySpace&                 ks,
    int                             num_transactions,
    int                             num_threads = 1,
    double                          hot_prob    = 0.0,
    int                             hot_size    = 10);