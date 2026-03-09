#pragma once // avoid multiple inclusion(same header file included in different source files)

#include <unordered_map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <regex>
#include "rocksdb_wrapper.h"

using namespace std;
using json = nlohmann::json;

enum OpType {
    READ,
    WRITE,
    ASSIGN,
    COMMIT
};

// A single field assignment: var["field"] = var["field"] +/- value
// e.g. from_acc["balance"] = from_acc["balance"] - 1
struct AssignOp {
    std::string lhs_var;    // "from_acc"
    std::string lhs_field;  // "balance"
    std::string rhs_var;    // "from_acc"  (same or different var)
    std::string rhs_field;  // "balance"
    int         delta;      // -1, +1, +5 etc. (0 if just copy)
    bool        is_extract; // true for: o_id = d["next_o_id"] (no arithmetic, just extract)
    std::string extract_var;   // for is_extract: target plain variable name
    std::string extract_field; // for is_extract: source field
};

// An operation in a transaction template: READ, WRITE, ASSIGN, or COMMIT
struct Op{
    OpType type;
    string var;      // for READ/WRITE: variable name (e.g. "from_acc")
    string key_param; // for READ/WRITE: input parameter name (e.g. "FROM_KEY")
    AssignOp assign;
};

// key type in TxnTemplate
struct ParamInfo{
    std::string name; // parameter name like "FROM_KEY"
    std::string type; // key type like "account", "customer", etc.
};

struct TxnTemplate{
    vector<ParamInfo> input_params; // input keys
    vector<Op> ops; // operations in the transaction template
};

struct Workload
{
    vector<TxnTemplate> templates; // transaction templates
};

// Parsed workload + keyspace needed for instantiation
Workload parseWorkloadFile(const std::string& filename);

void Trim(string &s); // helper function to trim leading and trailing spaces from a string

// categorize keys during loading



// Keyspace: grouped by type
struct KeySpace {
    std::vector<std::string> warehouse_keys;   // W_*
    std::vector<std::string> district_keys;    // D_*
    std::vector<std::string> stock_keys;       // S_*
    std::vector<std::string> customer_keys;    // C_*
    std::vector<std::string> account_keys;     // A_*
};

// Determine key type from prefix
std::string getKeyType(const std::string& key);


// Returns the populated keyspace after loading
KeySpace loadInputFile(RocksDBWrapper& db, const std::string& filename);

std::string inferTypeFromParam(const std::string& param); // infer key type from param name in workload file

// parse workload file and return the transaction templates
Workload parseWorkloadFile(const std::string& filename);

// For debugging: print the parsed workload
void printWorkload(const Workload& workload);

// test 2: verify random key selection from keyspace + actual execution
void testWorkloadExecution(const Workload& workload, const KeySpace& ks, RocksDBWrapper& db);
