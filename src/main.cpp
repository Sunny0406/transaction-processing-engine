#include <argparse/argparse.hpp>
#include <fstream>
#include <input_parse.h>
#include "transaction.h"
#include "transaction_manager.h"
#include "rocksdb_wrapper.h"
#include "input_parse.h"
#include <iostream>
#include <chrono>
#include <random>
#include "workload_runner.h"

using namespace std;
using json = nlohmann::json;

void verifyLoading(RocksDBWrapper& db, const KeySpace& ks) {
    std::cout << "=== Verifying Data Load ===\n";
    std::cout << "  Accounts:   " << ks.account_keys.size()   << "\n";
    std::cout << "  Warehouses: " << ks.warehouse_keys.size() << "\n";
    std::cout << "  Districts:  " << ks.district_keys.size()  << "\n";
    std::cout << "  Stocks:     " << ks.stock_keys.size()     << "\n";
    std::cout << "  Customers:  " << ks.customer_keys.size()  << "\n";
    auto check = [&](const std::vector<std::string>& keys, const std::string& type) {
        if (keys.empty()) { std::cout << type << ": EMPTY!\n"; return; }
        try {
            json val = db.get(keys[0]);
            std::cout << type << " [" << keys[0] << "] = " << val.dump() << "\n";
        } catch (const std::exception& e) {
            std::cout << type << " ERROR: " << e.what() << "\n";
        }
    };
    check(ks.account_keys,   "Account");
    check(ks.warehouse_keys, "Warehouse");
    check(ks.district_keys,  "District");
    check(ks.stock_keys,     "Stock");
    check(ks.customer_keys,  "Customer");
}

auto main(int argc, char* argv[]) -> int {

    // ── CLI argument parsing ──────────────────────────────────────────────────
    argparse::ArgumentParser program("txn-benchmark");

    program.add_argument("--workload")
        .default_value(std::string("workload1.txt"))
        .help("workload file path");

    program.add_argument("--input")
        .default_value(std::string("input1.txt"))
        .help("input data file path");

    program.add_argument("--cc")
        .default_value(std::string("both"))
        .help("concurrency control: occ | 2pl | both");

    program.add_argument("--threads")
        .default_value(std::string("1,2,4,8"))
        .help("comma-separated thread counts, e.g. 1,2,4,8");

    program.add_argument("--contention")
        .default_value(std::string("0.0,0.2,0.5,0.8,1.0"))
        .help("comma-separated hot_prob values, e.g. 0.0,0.5,1.0");

    program.add_argument("--hotsize")
        .default_value(10)
        .scan<'i', int>()
        .help("number of keys in the hotset");

    program.add_argument("--txns")
        .default_value(10000)
        .scan<'i', int>()
        .help("total transactions per run");

    program.add_argument("--fixed-hot")
        .default_value(0.5)
        .scan<'g', double>()
        .help("fixed hot_prob for Experiment A (throughput vs threads)");

    program.add_argument("--fixed-threads")
        .default_value(4)
        .scan<'i', int>()
        .help("fixed thread count for Experiment B (throughput vs contention)");

    program.add_argument("--verify")
        .default_value(false)
        .implicit_value(true)
        .help("print loaded data spot-check");

    try {
        program.parse_args(argc, argv);
    } catch (const std::runtime_error& err) {
        std::cerr << err.what() << "\n\n" << program;
        return 1;
    }

    // ── Parse thread list ─────────────────────────────────────────────────────
    auto parseInts = [](const std::string& s) {
        std::vector<int> result;
        std::stringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, ','))
            result.push_back(std::stoi(tok));
        return result;
    };

    auto parseDoubles = [](const std::string& s) {
        std::vector<double> result;
        std::stringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, ','))
            result.push_back(std::stod(tok));
        return result;
    };

    std::string cc_mode_str  = program.get<std::string>("--cc");
    std::string workload_file = program.get<std::string>("--workload");
    std::string input_file    = program.get<std::string>("--input");
    int         num_txns      = program.get<int>("--txns");
    int         hot_size      = program.get<int>("--hotsize");
    double      fixed_hot     = program.get<double>("--fixed-hot");
    int         fixed_threads = program.get<int>("--fixed-threads");
    bool        do_verify     = program.get<bool>("--verify");

    auto thread_list     = parseInts(program.get<std::string>("--threads"));
    auto contention_list = parseDoubles(program.get<std::string>("--contention"));

    bool run_occ = (cc_mode_str == "occ"  || cc_mode_str == "both");
    bool run_2pl = (cc_mode_str == "2pl"  || cc_mode_str == "both");

    // ── Print configuration ───────────────────────────────────────────────────
    std::cout << "=== Configuration ===\n";
    std::cout << "  Workload  : " << workload_file << "\n";
    std::cout << "  Input     : " << input_file    << "\n";
    std::cout << "  CC mode   : " << cc_mode_str   << "\n";
    std::cout << "  Txns/run  : " << num_txns      << "\n";
    std::cout << "  Hot size  : " << hot_size       << "\n";
    std::cout << "  Threads   : " << program.get<std::string>("--threads")     << "\n";
    std::cout << "  Contention: " << program.get<std::string>("--contention")  << "\n";

    // ── 1. Open DB and load data ──────────────────────────────────────────────
    RocksDBWrapper db("./mydb");
    KeySpace ks = loadInputFile(db, input_file);
    std::cout << "\nLoaded " << ks.account_keys.size() << " accounts, "
                             << ks.district_keys.size() << " districts\n";
    if (do_verify) verifyLoading(db, ks);

    // ── 2. Parse workload ─────────────────────────────────────────────────────
    Workload workload = parseWorkloadFile(workload_file);

    // ── Experiment A: Throughput vs Threads ───────────────────────────────────
    std::cout << "\n=== Experiment A: Throughput vs Threads"
              << " (hot_prob=" << fixed_hot << ") ===\n";

    for (int t : thread_list) {
        if (run_occ) {
            std::cout << "\n[OCC | threads=" << t << "]\n";
            TransactionManager mgr(db.getDB(), CCMode::OCC);
            RunWorkload(mgr, workload.templates, ks, num_txns, t, fixed_hot, hot_size);
        }
        if (run_2pl) {
            std::cout << "\n[2PL | threads=" << t << "]\n";
            TransactionManager mgr(db.getDB(), CCMode::TWO_PL);
            RunWorkload(mgr, workload.templates, ks, num_txns, t, fixed_hot, hot_size);
        }
    }

    // ── Experiment B: Throughput vs Contention ────────────────────────────────
    std::cout << "\n=== Experiment B: Throughput vs Contention"
              << " (threads=" << fixed_threads << ") ===\n";

    for (double p : contention_list) {
        if (run_occ) {
            std::cout << "\n[OCC | hot_prob=" << p << "]\n";
            TransactionManager mgr(db.getDB(), CCMode::OCC);
            RunWorkload(mgr, workload.templates, ks, num_txns, fixed_threads, p, hot_size);
        }
        if (run_2pl) {
            std::cout << "\n[2PL | hot_prob=" << p << "]\n";
            TransactionManager mgr(db.getDB(), CCMode::TWO_PL);
            RunWorkload(mgr, workload.templates, ks, num_txns, fixed_threads, p, hot_size);
        }
    }

    return 0;
}