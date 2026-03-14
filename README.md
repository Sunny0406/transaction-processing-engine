# Transaction Benchmark: OCC vs Conservative 2PL

A multi-threaded transaction processing layer built on top of [RocksDB](https://rocksdb.org), implementing and comparing two concurrency control protocols: **Optimistic Concurrency Control (OCC)** and **Conservative Two-Phase Locking (Conservative 2PL)**.

---

## Dependencies

| Dependency | Purpose |
|---|---|
| [RocksDB](https://github.com/facebook/rocksdb) | Embedded key-value storage layer |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON value serialization / deserialization |
| [argparse](https://github.com/p-ranav/argparse) | CLI argument parsing |
| CMake ≥ 3.16 | Build system |
| C++17 compiler | `g++` or `clang++` |

### Installing RocksDB (macOS)

```bash
brew install rocksdb
```

### Installing RocksDB (Ubuntu/Debian)

```bash
sudo apt-get install librocksdb-dev
```

---

## Build Instructions

```bash
# 1. Clone the repository
git clone <your-repo-url>
cd db_final_project

# 2. Create and enter the build directory
mkdir build && cd build

# 3. Configure with CMake
cmake ..

# 4. Compile
make -j$(nproc)
```

The compiled binary will be at `./build/txn-benchmark`.

---

## Running the Benchmark

### Basic usage

```bash
./build/txn-benchmark --workload <workload_file> --input <input_file> [options]
```

### Run both workloads with default settings

```bash
# Workload 1 — Bank Transfer (500 accounts)
./build/txn-benchmark --workload workload1.txt --input input1.txt

# Workload 2 — Order / Payment (TPC-C inspired, 80 districts)
./build/txn-benchmark --workload workload2.txt --input input2.txt
```

### Run with a specific concurrency control mode

```bash
# OCC only
./build/txn-benchmark --workload workload1.txt --input input1.txt --cc occ

# Conservative 2PL only
./build/txn-benchmark --workload workload1.txt --input input1.txt --cc 2pl

# Both (default)
./build/txn-benchmark --workload workload1.txt --input input1.txt --cc both
```

---

## Setting Number of Threads

Use `--threads` with a comma-separated list of thread counts. Each value runs a separate experiment under **Experiment A** (throughput vs threads):

```bash
./build/txn-benchmark --workload workload1.txt --input input1.txt --threads 1,2,4,8
```

Use `--fixed-threads` to set the thread count used in **Experiment B** (throughput vs contention):

```bash
./build/txn-benchmark --workload workload1.txt --input input1.txt --fixed-threads 4
```

---

## Setting Contention Level

Contention is controlled by two parameters:

### `--contention` — hot probability sweep (Experiment B)

A comma-separated list of `hot_prob` values between `0.0` and `1.0`. With probability `p`, each transaction key is drawn from a small hotset; with probability `1 - p`, it is drawn uniformly from the full keyspace. Higher `p` = higher contention.

```bash
./build/txn-benchmark --workload workload1.txt --input input1.txt --contention 0.0,0.2,0.5,0.8,1.0
```

### `--hotsize` — number of hot keys

Sets the size of the hotset (default: 10). Smaller hotsets increase contention for a given `hot_prob`.

```bash
./build/txn-benchmark --workload workload1.txt --input input1.txt --hotsize 5
```

### `--fixed-hot` — fixed hot probability for Experiment A

Sets the contention level used while sweeping thread counts (default: `0.5`):

```bash
./build/txn-benchmark --workload workload1.txt --input input1.txt --fixed-hot 0.5
```

---

## All CLI Options

| Flag | Default | Description |
|---|---|---|
| `--workload` | `workload1.txt` | Path to the workload definition file |
| `--input` | `input1.txt` | Path to the initial data file |
| `--cc` | `both` | Concurrency control mode: `occ`, `2pl`, or `both` |
| `--threads` | `1,2,4,8` | Comma-separated thread counts for Experiment A |
| `--contention` | `0.0,0.2,0.5,0.8,1.0` | Comma-separated `hot_prob` values for Experiment B |
| `--hotsize` | `10` | Number of keys in the hotset |
| `--txns` | `10000` | Number of committed transactions per run |
| `--fixed-hot` | `0.5` | Fixed `hot_prob` used in Experiment A |
| `--fixed-threads` | `4` | Fixed thread count used in Experiment B |
| `--verify` | `false` | Print a spot-check of loaded data before running |

---

## Example: Full Reproduction of Paper Results

```bash
# Workload 1 — full sweep
./build/txn-benchmark \
  --workload workload1.txt \
  --input input1.txt \
  --cc both \
  --threads 1,2,4,8 \
  --contention 0.0,0.2,0.5,0.8,1.0 \
  --hotsize 10 \
  --txns 10000 \
  --fixed-hot 0.5 \
  --fixed-threads 4

# Workload 2 — full sweep
./build/txn-benchmark \
  --workload workload2.txt \
  --input input2.txt \
  --cc both \
  --threads 1,2,4,8 \
  --contention 0.0,0.2,0.5,0.8,1.0 \
  --hotsize 10 \
  --txns 10000 \
  --fixed-hot 0.5 \
  --fixed-threads 4
```

---

## Output Format

Each run prints a results block like:

```
=== RunWorkload Results ===
  Threads      : 4
  Transactions : 10029
  Committed    : 10000
  Aborted      : 29  (0.289161%)
  Lock retries : 0
  Elapsed      : 0.274772s
  Throughput   : 36393.8 txns/sec
  Avg latency  : 0.0240953 ms
```

- **Transactions** — total attempted (committed + aborted)
- **Committed** — successfully committed transactions
- **Aborted** — OCC validation failures (always 0 for 2PL)
- **Lock retries** — failed `AcquireAll()` calls requiring backoff (always 0 for OCC)
- **Throughput** — committed transactions per second
- **Avg latency** — average wall-clock time per committed transaction (ms)

---

## Project Structure

```
db_final_project/
├── main.cpp                  # Entry point, CLI parsing, experiment loops
├── transaction.h             # Transaction class (read set, write buffer, status)
├── transaction_manager.h/.cpp  # Begin / Read / Write / Commit / Abort, OCC & 2PL logic
├── lock_manager.h/.cpp       # Conservative 2PL lock table, AcquireAll / ReleaseAll
├── workload_runner.h/.cpp    # Multi-threaded workload execution engine
├── input_parse.h/.cpp        # Input file parser, KeySpace definition
├── rocksdb_wrapper.h/.cpp    # Thin wrapper around rocksdb::DB
├── workload1.txt             # Bank transfer workload definition
├── workload2.txt             # Order / Payment workload definition
├── input1.txt                # 500 account records
├── input2.txt                # Warehouse / district / stock / customer records
└── CMakeLists.txt
```

---

## Notes

- The database directory `./mydb` is created automatically on first run. Delete it between runs if you want a clean state.
- Do **not** submit compiled binaries or the `build/` or `mydb/` directories.
- Absolute throughput numbers will vary by machine; focus on **relative trends** between OCC and 2PL under the same environment.
