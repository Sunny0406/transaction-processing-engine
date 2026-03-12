#include "input_parse.h"
#include "transaction_manager.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <regex>
#include <random>

using namespace std;

// Determine key type from prefix
std::string getKeyType(const std::string& key) {
    if (key[0] == 'W') return "warehouse";
    if (key[0] == 'D') return "district";
    if (key[0] == 'S') return "stock";
    if (key[0] == 'C') return "customer";
    if (key[0] == 'A') return "account";
    return "unknown";
}

void Trim(string &s) {
    // remove leading spaces
    s.erase(s.begin(), find_if(s.begin(), s.end(), [](unsigned char ch) {
        return !isspace(ch);
    }));

    // remove trailing spaces
    s.erase(find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
        return !isspace(ch);
    }).base(), s.end());
}

// Normalize {name: "x", balance: 0} → {"name":"x","balance":0}
json parseValue(const std::string& raw) {
    std::string s = raw;
    static const std::regex keyRegex(R"(([{,]\s*)(\w+)(\s*:))");
    s = std::regex_replace(s, keyRegex, R"($1"$2"$3)");
    return json::parse(s);
}

KeySpace loadInputFile(RocksDBWrapper& db, const std::string& filename) {
    KeySpace ks;
    std::ifstream file(filename);
    std::string line;

    std::getline(file, line); // skip "INSERT"

    while (std::getline(file, line)) {
        if (line == "END" || line.empty()) break;

        // Parse:  KEY: A_1, VALUE: {name: "Account-1", balance: 153}
        size_t kStart = line.find("KEY: ") + 5;
        size_t vMark  = line.find(", VALUE: ");
        std::string key   = line.substr(kStart, vMark - kStart);
        std::string value = line.substr(vMark + 9);

        json j = parseValue(value);

        // 1. Store in RocksDB
        db.put(key, j);

        // 2. Index by type (for random key selection later)
        char prefix = key[0];
        if      (prefix == 'W') ks.warehouse_keys.push_back(key);
        else if (prefix == 'D') ks.district_keys.push_back(key);
        else if (prefix == 'S') ks.stock_keys.push_back(key);
        else if (prefix == 'C') ks.customer_keys.push_back(key);
        else if (prefix == 'A') ks.account_keys.push_back(key);
    }
    return ks;
}

//  ==== workload parser: read transaction templates from filel ====

// update:
// returns vector<ParamInfo> instead of vector<string> to include key type info for random binding later
std::vector<ParamInfo> parseTransactionHeader(const std::string& line) {
    std::vector<ParamInfo> params;

    size_t start = line.find("INPUTS:") + 7;
    size_t end   = line.find(")");
    std::stringstream ss(line.substr(start, end - start));
    std::string token;

    while (std::getline(ss, token, ',')) {
        // trim whitespace
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);

        ParamInfo p;
        p.name = token;
        p.type = inferTypeFromParam(token);  // ← infer type here
        params.push_back(p);
    }
    return params;
}

// add this helper and call it on every line before parseLine()
std::string sanitizeLine(const std::string& line){
    std::string result;
    for (unsigned char c : line){
        if (c < 128) result += c; // keep ASCII, remove non-ASCII
        else {
            // replace non-ASCII(curly quotes) with straight quotes
            result += '"';
        }
    }
    return result;
}


// Parse one line into an Op.
// Returns false if line is not a recognized op (blank, etc.)
bool parseLine(const std::string& line, Op& op) {
    // --- COMMIT ---
    if (line == "COMMIT") {
        op.type = OpType::COMMIT;
        return true;
    }

    // --- READ: var = READ(KEY_PARAM) ---
    // e.g.  "from_acc = READ(FROM_KEY)"
    {
        static const std::regex readRe(R"re((\w+)\s*=\s*READ\((\w+)\))re");
        std::smatch m;
        if (std::regex_match(line, m, readRe)) {
            op.type      = OpType::READ;
            op.var       = m[1];   // "from_acc"
            op.key_param = m[2];   // "FROM_KEY"
            return true;
        }
    }

    // --- WRITE: WRITE(KEY_PARAM, var) ---
    // e.g. "WRITE(FROM_KEY, from_acc)"
    {
        static const std::regex writeRe(R"re(WRITE\((\w+),\s*(\w+)\))re");
        std::smatch m;
        if (std::regex_match(line, m, writeRe)) {
            op.type      = OpType::WRITE;
            op.key_param = m[1];  // "FROM_KEY"
            op.var       = m[2];  // "from_acc"
            return true;
        }
    }

    // --- ASSIGN with arithmetic: var["field"] = var["field"] +/- N ---
    // e.g. "from_acc["balance"] = from_acc["balance"] - 1"
    {
        static const std::regex arithRe(R"re((\w+)\["(\w+)"\]\s*=\s*(\w+)\["(\w+)"\]\s*([+\-])\s*(\d+))re");
        std::smatch m;
        if (std::regex_match(line, m, arithRe)) {
            op.type              = OpType::ASSIGN;
            op.assign.lhs_var    = m[1];
            op.assign.lhs_field  = m[2];
            op.assign.rhs_var    = m[3];
            op.assign.rhs_field  = m[4];
            int val              = std::stoi(m[6]);
            op.assign.delta      = (m[5] == "-") ? -val : val;
            op.assign.is_extract = false;
            return true;
        }
    }

    // --- EXTRACT: plain_var = var["field"] ---
    // e.g. "o_id = d["next_o_id"]"
    {
        static const std::regex extractRe(R"re((\w+)\s*=\s*(\w+)\["(\w+)"\])re");
        std::smatch m;
        if (std::regex_match(line, m, extractRe)) {
            op.type                 = OpType::ASSIGN;
            op.assign.is_extract    = true;
            op.assign.extract_var   = m[1];  // "o_id"
            op.assign.rhs_var       = m[2];  // "d"
            op.assign.extract_field = m[3];  // "next_o_id"
            return true;
        }
    }

    // --- ASSIGN using plain var: var["field"] = plain_var + N ---
    // e.g. "d["next_o_id"] = o_id + 1"
    {
        static const std::regex plainRe(
            R"re((\w+)\["(\w+)"\]\s*=\s*(\w+)\s*([+\-])\s*(\d+))re");
        std::smatch m;
        if (std::regex_match(line, m, plainRe)) {
            op.type              = OpType::ASSIGN;
            op.assign.lhs_var    = m[1];
            op.assign.lhs_field  = m[2];
            op.assign.rhs_var    = m[3];  // plain var name like "o_id"
            op.assign.rhs_field  = "";    // no field — it's a plain int var
            int val              = std::stoi(m[5]);
            op.assign.delta      = (m[4] == "-") ? -val : val;
            op.assign.is_extract = false;
            return true;
        }
    }

    return false; // blank line or unrecognized
}

Workload parseWorkloadFile(const std::string& filename) {
    Workload workload;
    std::ifstream file(filename);
    std::string line;

    std::getline(file, line); // skip "WORKLOAD"

    while (std::getline(file, line)) {

        if (line == "END") break;
        if (line.find("TRANSACTION") == std::string::npos) continue;

        TxnTemplate txn;
        txn.input_params = parseTransactionHeader(line);  // ← now returns ParamInfo

        while (std::getline(file, line)) {
            std::string trimmed = sanitizeLine(line); // remove non-ASCII chars and trim whitespace
            trimmed.erase(0, trimmed.find_first_not_of(" \t"));
            trimmed.erase(trimmed.find_last_not_of(" \t") + 1);
            Op op;
            if (parseLine(trimmed, op)) {
                txn.ops.push_back(op);
                if (op.type == OpType::COMMIT) break;
            }
        }
        workload.templates.push_back(txn);
    }
    return workload;
}


// bind real keys to param names
void executeTxn(const TxnTemplate& tmpl, const map<string, string>& binding,
                TransactionManager& txn_mgr) {
    // binding: e.g. "FROM_KEY" → "A_123", "TO_KEY" → "A_456"
    // which means key to key_param mapping for this txn instance

    // local state during txn
    map<string, json> vars; // json objects for variables like "from_acc", "to_acc", etc.
    map<string, int> plain_vars; // for plain int variables like "o_id"

    // Begin the transaction by using Begin()
    Transaction* txn = txn_mgr.Begin();



    for (auto& op:tmpl.ops){
        // key_param is key
        // var is value
        switch(op.type){
            case OpType::READ: {
                string key = binding.at(op.key_param); // e.g. "FROM_KEY" → "A_123"
                // vars[op.var] = db.get(key);            // e.g. from_acc = db.get("A_123")

                // use TransactionManager's Read() to read and record in read_set for OCC validation later
                bool success = txn_mgr.Read(txn, key, vars[op.var]); // read from db and record in txn's read set for OCC
                if (!success) {
                    throw std::runtime_error("Failed to read from transaction manager");
                }

                break;
            }
            case OpType::WRITE: {
                // WRITE(key_param, var) -> write local value(var) to db at key(key_param)
                string key = binding.at(op.key_param); // e.g. "FROM_KEY" →
                //db.put(key, vars[op.var]);             // e.g. db.put("A_123", from_acc)
                // without CC, we can write immediately;
                // with CC, we would buffer the write and apply at commit time after validation
                txn_mgr.Write(txn, key, vars[op.var]);
                
                break;
            }
            case OpType::ASSIGN: {
                auto& a = op.assign;
                if (a.is_extract) {
                    // extract: plain_var = var["field"]
                    // o_id = d["next_o_id"]
                    // which means extract "next_o_id" field from d and store in plain variable o_id
                    plain_vars[a.extract_var] = vars.at(a.rhs_var)[a.extract_field].get<int>(); // e.g. o_id = d["next_o_id"]
                } else if (a.rhs_field.empty()) {
                    // d["next_o_id"] = o_id + 1 -> rhs is plain var + delta
                    int rhs = plain_vars.at(a.rhs_var) + a.delta; // e.g. o_id + 1
                    vars.at(a.lhs_var)[a.lhs_field] = rhs; // e.g. d["next_o_id"] = o_id + 1
                } else {
                    // from_acc["balance"] = from_acc["balance"] - 1 -> rhs is var["field"] +/- delta
                    int rhs = vars.at(a.rhs_var)[a.rhs_field].get<int>() + a.delta; // e.g. from_acc["balance"] - 1
                    vars.at(a.lhs_var)[a.lhs_field] = rhs; // e.g. from_acc["balance"] = from_acc["balance"] - 1
                }

                break;
            }
            case OpType::COMMIT:{
                txn_mgr.Commit(txn); // for now just commit immediately without validation since we haven't implemented CC yet
                break; // let CC layer handle commit logic (validation, flushing writes, etc.)
            }
        }   
    
    }
}

// For debugging: print the parsed workload
// test 1: verify workload parsing correctness by printing the parsed structure
void printWorkload(const Workload& workload) {
    std::cout << "Parsed Workload:\n";
    std::cout << "Number of transaction templates: " << workload.templates.size() << "\n";
    for (int i=0; i<workload.templates.size(); i++) {
        const auto& tmpl = workload.templates[i];
        std::cout << "Template " << i << ":\n";

        // print input params
        std::cout << "  Input params: ";
        for (const auto& param : tmpl.input_params) {
            std::cout << param.name << "(" << param.type << ") ";
        }

        // print ops
        std::cout << "\n  Ops:\n";
        for (const auto& op : tmpl.ops) {
            switch (op.type) {
                case OpType::READ:
                    std::cout << "    READ: " << op.var << " = READ(" << op.key_param << ")\n";
                    break;
                case OpType::WRITE:
                    std::cout << "    WRITE: WRITE(" << op.key_param << ", " << op.var << ")\n";
                    break;
                case OpType::ASSIGN:
                    if (op.assign.is_extract) {
                        std::cout << "    ASSIGN(extract): " << op.assign.extract_var
                                  << " = " << op.assign.rhs_var
                                  << "[\"" << op.assign.extract_field << "\"]\n";
                    } else if (op.assign.rhs_field.empty()) {
                        std::cout << "    ASSIGN(plain): " << op.assign.lhs_var
                                  << "[\"" << op.assign.lhs_field << "\"] = "
                                  << op.assign.rhs_var << " " 
                                  << (op.assign.delta >= 0 ? "+" : "-") << " "
                                  << abs(op.assign.delta) << "\n";
                    } else {
                        std::cout << "    ASSIGN(arith): " << op.assign.lhs_var
                                  << "[\"" << op.assign.lhs_field << "\"] = "
                                  << op.assign.rhs_var << "[\"" << op.assign.rhs_field << "\"]\n";
                    }
                    break;
                case OpType::COMMIT:
                    std::cout << "    COMMIT\n";
                    break;
            }
        }
    }
}


// add inferTypeFromParam() to map the naming convention in the workload file
// to keyspace categories

std::string inferTypeFromParam(const std::string& param) {
    // for debugging "all types unknown" issue: print the param name being inferred
    //std::cout << "[DEBUG] Inferring type for param: " << param << "\n";


    // Print exact bytes to reveal hidden characters
    //std::cout << "v2 - [DEBUG] param='" << param << "' length=" << param.size() << " bytes: ";
    //for (unsigned char c : param) {
    //    std::cout << std::hex << (int)c << " ";
    //}
    //std::cout << std::dec << "\n";

    // check by prefix convention in worrkload file
    if (param.find("W_KEY") != std::string::npos) return "warehouse";
    if (param.find("D_KEY") != std::string::npos) return "district";
    if (param.find("S_KEY") != std::string::npos) return "stock";
    if (param.find("C_KEY") != std::string::npos) return "customer";
    // workload1 uses FROM_KEY/TO_KEY -> account keys
    if (param.find("FROM_KEY") != std::string::npos 
    || param.find("TO_KEY") != std::string::npos) return "account";
    
    // std::cout << "[DEBUG] unknown param type for: " << param << "\n";
    
    return "unknown";
}


// i may need a function, InstatiateTransaction()
// to help me randomly pick real keys from the keyspace by given templates
// with optional hotset support for contention control
// put this function in input_parse.cpp since it's closely related to workload and keyspace parsing ?? 
std::map<std::string, std::string> instantiateTransaction(
    const TxnTemplate& tmpl,
    const KeySpace& ks,
    std::mt19937& rng,
    double hot_prob,
    int    hot_size)
{
    std::map<std::string, std::string> binding;

    auto pickKey = [&](const std::vector<std::string>& keys) -> std::string {
        std::uniform_real_distribution<> coin(0.0, 1.0);
        std::uniform_int_distribution<>  hot(0,  std::min(hot_size, (int)keys.size()) - 1);
        std::uniform_int_distribution<>  full(0, (int)keys.size() - 1);
        return (coin(rng) < hot_prob) ? keys[hot(rng)] : keys[full(rng)];
    };

    for (auto& p : tmpl.input_params) {  // p.name, p.type both available
        if      (p.type == "warehouse") binding[p.name] = pickKey(ks.warehouse_keys);
        else if (p.type == "district")  binding[p.name] = pickKey(ks.district_keys);
        else if (p.type == "stock")     binding[p.name] = pickKey(ks.stock_keys);
        else if (p.type == "customer")  binding[p.name] = pickKey(ks.customer_keys);
        else if (p.type == "account")   binding[p.name] = pickKey(ks.account_keys);
        else
            throw std::runtime_error("Unknown param type for: " + p.name);
    }
    return binding;
}


// test 2: verify random key selection from keyspace + actual execution
void testWorkloadExecution(const Workload& workload, const KeySpace& ks, RocksDBWrapper& db) {
    std::cout << "\n === Testing Workload Execution ===\n";
    std::mt19937 rng(42); // fixed seed for reproducibility

    for (int i=0; i<workload.templates.size(); i++){
        const auto& tmpl = workload.templates[i];
        std::cout << "\n--- Running Template " << i << " ---\n";
        // step 1: show param->key mapping first
        std::cout << "Param types:\n";
        for (auto& p:tmpl.input_params){
            std::cout << "Param: " << p.name << " -> " << p.type << "\n";
        }

        // step 2: randomly binding real keys to using inferred types
        std::map<std::string, std::string> binding;
        try{
            binding = instantiateTransaction(tmpl, ks, rng, 0.5, 5); // 50% hot prob, hotset size 5
        } catch (const std::exception& e) {
            std::cout << "Error during instantiation: " << e.what() << "\n";
            continue;
        }

        // step 3: show the concrete key binding chosen
        std::cout << "Key binding:\n";
        for (auto& [param, key] : binding) {
            std::cout << "  " << param << " -> " << key << "\n";
        }

        //step 4: print BEFORE state from DB
        for (auto& [param, key]:binding){
            try{
                json val = db.get(key);
                std::cout << "  [" << key << "] = " << val.dump() << "\n";
            } catch (const std::exception& e) {
                std::cout << "  [" << key << "] ERROR: " << e.what() << "\n";
            }
        }

        //step 5: execute the transaction
        TransactionManager txn_mgr(db.getDB(), CCMode::OCC); // create a transaction manager with a test RocksDB instance
        TransactionManager& dummy_txn_mgr = txn_mgr; // create a dummy transaction manager
        try{
            executeTxn(tmpl, binding, /*txn_mgr=*/dummy_txn_mgr); // use a dummy txn manager for now since we haven't implemented CC yet
            std::cout << "Execution: OK\n";
        } catch (const std::exception& e) {
            std::cout << "Execution: ERROR - " << e.what() << "\n";
        }

        //step 6: print AFTER state from DB to verify changes
        std::cout << "AFTER state:\n";
        bool any_changed = false;
        for (auto& [param, key]:binding){
            json val = db.get(key);
            std::cout << "  [" << key << "] = " << val.dump() << "\n";
            // Note: in a real test, we would want to compare against expected values
            // to automatically verify correctness. Here we just print the state for manual inspection.
        }
    }
}