#include <argparse/argparse.hpp> 
#include <fstream> // for file I/O
#include <input_parse.h> // for workload parsing

using namespace std;
using json = nlohmann::json;



// testing whether we can open the DB and load the input file correctly
// After loading, read back a few keys and print them
void verifyLoading(RocksDBWrapper& db, const KeySpace& ks) {
    std::cout << "=== Verifying Data Load ===\n";
    std::cout << "Key counts:\n";
    std::cout << "  Accounts:   " << ks.account_keys.size()  << "\n";
    std::cout << "  Warehouses: " << ks.warehouse_keys.size() << "\n";
    std::cout << "  Districts:  " << ks.district_keys.size()  << "\n";
    std::cout << "  Stocks:     " << ks.stock_keys.size()     << "\n";
    std::cout << "  Customers:  " << ks.customer_keys.size()  << "\n";

    // Spot-check: read first key of each type and print
    auto check = [&](const std::vector<std::string>& keys, const std::string& type) {
        if (keys.empty()) { std::cout << type << ": EMPTY!\n"; return; }
        try {
            json val = db.get(keys[0]);
            std::cout << type << " [" << keys[0] << "] = " << val.dump() << "\n";
        } catch (const std::exception& e) {
            std::cout << type << " [" << keys[0] << "] ERROR: " << e.what() << "\n";
        }
    };

    check(ks.account_keys,  "Account");
    check(ks.warehouse_keys,"Warehouse");
    check(ks.district_keys, "District");
    check(ks.stock_keys,    "Stock");
    check(ks.customer_keys, "Customer");
}



auto main(int argc, char *argv[]) -> int {


    // argparse::ArgumentParser program("txn-benchmark");
    // program.add_argument("-cc").default_value("occ").help("Concurrency control algorithm to use (occ, 2pl)");
    // program.add_argument("--threads").default_value(2).scan<'i', int>().help("number of worker threads");
    // program.add_argument("--contention").default_value(0.0).scan<'g', double>().help("hotset probability p (0.0 to 1.0)");
    // program.add_argument("--hotset").default_value(10).scan<'i', int>().help("hotset size (number of hot keys)");
    // program.add_argument("--workload").required().help("workload file path");


    // try {
    // program.parse_args(argc, argv); // parse_arg is validating the arguments and will throw if any error occurs
    // // actual parsing work is done by calling parse_args_internal, which is called by parse_args. 
    // // parse_args_internal will call the action of each argument, which will set the value of the argument and mark it as used. 
    // // If any error occurs during parsing, it will throw a runtime_error with a message describing the error.
    // } catch (const std::runtime_error &err) {
    // std::cerr << err.what() << std::endl;}

    // string workload_file = program.get<std::string>("--workload");
    // string cc_algorithm = program.get<std::string>("-cc");
    // int num_threads = program.get<int>("--threads");
    // double contention = program.get<double>("--contention");
    // int hotset_size = program.get<int>("--hotset");
    // bool debug_mode = program.get<bool>("--debug");

    // Open DB
    RocksDBWrapper db("./mydb");

    // 1. LOADING PHASE — single thread, no concurrency control
    // KeySpace ks = loadInputFile(db, "input1.txt");
    KeySpace ks2 = loadInputFile(db, "input2.txt");

    // std::cout << "Loaded "
    //           << ks.account_keys.size() << " accounts, "
    //           << ks.district_keys.size() << " districts\n";
    std::cout << "Loaded "
              << ks2.account_keys.size() << " accounts, "
              << ks2.district_keys.size() << " districts\n";

    verifyLoading(db, ks2);

    // 2. Parse workload
    Workload workload = parseWorkloadFile("workload2.txt");

    // 3. Test 1: verify parsing structure + type inference
    std::cout << "\n=== Verifying Workload Parsing ===\n";
    printWorkload(workload);

    // 4. Test 2: verify random key selection from keyspace + actual execution
    testWorkloadExecution(workload, ks2, db);

    return 0;

}