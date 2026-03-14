set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

cmake_minimum_required(VERSION 3.16)
project(db_final_project LANGUAGES CXX)

# -------- Global settings --------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generate compile_commands.json (for clangd / IntelliSense)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default build type
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# -------- Sources --------
# (Recommended) Explicitly list sources to avoid missing files.
set(PROJECT_SOURCES
  src/main.cpp

  src/parser/input_parse.cpp

  src/storage/rocksdb_wrapper.cpp

  src/transaction/transaction_manager.cpp
  src/transaction/occ.cpp
  src/transaction/conservative_2pl.cpp
  src/transaction/lock_manager.cpp

  src/workload/workload_runner.cpp
)

# -------- Target --------
add_executable(txn-benchmark ${PROJECT_SOURCES})

# Include dirs (your headers + third_party)
target_include_directories(txn-benchmark PRIVATE
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/third_party
)

# -------- Threads --------
find_package(Threads REQUIRED)
target_link_libraries(txn-benchmark PRIVATE Threads::Threads)

# ---------- RocksDB: use system installed (brew/apt) ----------
find_path(ROCKSDB_INCLUDE_DIR rocksdb/db.h REQUIRED)
find_library(ROCKSDB_LIB rocksdb REQUIRED)

target_include_directories(txn-benchmark PRIVATE ${ROCKSDB_INCLUDE_DIR})
target_link_libraries(txn-benchmark PRIVATE ${ROCKSDB_LIB})

# Address sanitizer for debugging
# target_compile_options(txn-benchmark PRIVATE -fsanitize=address -fno-omit-frame-pointer)
# target_link_options(txn-benchmark PRIVATE -fsanitize=address)