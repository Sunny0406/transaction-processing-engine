# Transaction Processing Engine (OCC vs Conservative 2PL)

A multi-threaded transaction processing engine built on top of **RocksDB**, designed to evaluate and compare two concurrency control protocols:

- Optimistic Concurrency Control (OCC)
- Conservative Two-Phase Locking (2PL)

This project was developed for **CS 223 – Database Systems**.  
The system executes concurrent transactions over a key-value database and evaluates performance under different levels of contention and parallelism.

---

# System Architecture

The system consists of two layers:

## Storage Layer

The storage layer uses **RocksDB**, an embedded key-value store.

Records are stored in the following format:
Key -> Value

Where the value is stored as a JSON-style object.

Example:
C_1_1_2 -> {
"balance": 5000,
"payment_cnt": 0,
"ytd_payment": 0
}


RocksDB provides:

- persistent storage
- efficient key-value operations
- thread-safe database access

---

## Transaction Layer

The transaction layer implements the following functionality:

- Transaction begin
- Read operations
- Write operations
- Commit
- Concurrency control

It is responsible for ensuring correctness when multiple transactions execute concurrently.

---

# Concurrency Control Protocols

## Optimistic Concurrency Control (OCC)

OCC executes transactions without acquiring locks during execution.

Workflow:

1. Transactions read data and buffer writes locally
2. At commit time, validation checks whether conflicts occurred
3. If validation succeeds → commit
4. If validation fails → abort and retry

Characteristics:

- No lock overhead during execution
- High performance under low contention
- Higher abort rates under high contention

---

## Conservative Two-Phase Locking (2PL)

Conservative 2PL requires transactions to acquire all required locks before execution begins.

Workflow:

1. Determine all keys required by the transaction
2. Attempt to acquire locks for all keys
3. If any lock is unavailable → release locks and retry later
4. Execute transaction after locks are acquired
5. Release locks at commit

Characteristics:

- Prevents deadlocks
- May introduce waiting
- Lower abort rate than OCC under high contention

---

# Project Structure

RocksDB provides:

- persistent storage
- efficient key-value operations
- thread-safe database access

---

## Transaction Layer

The transaction layer implements the following functionality:

- Transaction begin
- Read operations
- Write operations
- Commit
- Concurrency control

It is responsible for ensuring correctness when multiple transactions execute concurrently.

---

# Concurrency Control Protocols

## Optimistic Concurrency Control (OCC)

OCC executes transactions without acquiring locks during execution.

Workflow:

1. Transactions read data and buffer writes locally
2. At commit time, validation checks whether conflicts occurred
3. If validation succeeds → commit
4. If validation fails → abort and retry

Characteristics:

- No lock overhead during execution
- High performance under low contention
- Higher abort rates under high contention

---

## Conservative Two-Phase Locking (2PL)

Conservative 2PL requires transactions to acquire all required locks before execution begins.

Workflow:

1. Determine all keys required by the transaction
2. Attempt to acquire locks for all keys
3. If any lock is unavailable → release locks and retry later
4. Execute transaction after locks are acquired
5. Release locks at commit

Characteristics:

- Prevents deadlocks
- May introduce waiting
- Lower abort rate than OCC under high contention

---

# Project Structure
