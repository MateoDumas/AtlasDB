# AtlasDB

[![CI](https://github.com/MateoDumas/AtlasDB/actions/workflows/ci.yml/badge.svg)](https://github.com/MateoDumas/AtlasDB/actions/workflows/ci.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

*[Leer en español](README.es.md)*

A small relational database written from scratch in C++20, with no dependencies beyond the
standard library: paged storage, persistent B+tree indexes, a SQL dialect, transactions,
write-ahead logging with crash recovery, integrity constraints, a CLI, and its **own binary
wire protocol** so that external applications (C++, Python, anything with sockets) can use it.

```
$ atlasdb library
atlas> SELECT a.name, COUNT(b.id) AS books, MIN(b.year) AS first
   ...>   FROM authors a LEFT JOIN books b ON b.author_id = a.id
   ...>   GROUP BY a.name ORDER BY books DESC, a.name;
+----------+-------+-------+
| name     | books | first |
+----------+-------+-------+
| Borges   |     2 |  1944 |
| Cortázar |     2 |  1951 |
| Ocampo   |     0 |  NULL |
+----------+-------+-------+
(3 rows)
atlas> EXPLAIN SELECT title FROM books WHERE year BETWEEN 1940 AND 1950;
| Project [title]                                                  |
|   -> IndexScan books USING books_year year BETWEEN 1940 AND 1950 |
atlas> INSERT INTO books (id, title, author_id, year) VALUES (5, 'Ghost', 99, 2000);
ERROR (constraint_violation): foreign key violation: books.author_id = 99 has no matching row in 'authors.id'
```

## Highlights

- **Storage engine**: 4 KiB slotted pages with CRC32 checksums, an LRU buffer pool, heap files
  with a lazily built free-space map, and overflow chains for rows larger than a page (up to 16 MiB).
- **Persistent B+tree indexes**: one file per index; nodes are loaded on first use and cold ones
  are evicted (LRU, per-index budget), so opening a 100k-row database takes ~18 ms instead of
  rebuilding indexes from the table, and memory stays bounded.
- **Crash safety**: redo-only WAL with **group commit** (concurrent commits share one `fsync`), page LSNs, and **atomic checkpoints
  through a doublewrite buffer** that repairs pages torn by a crash mid-checkpoint.
- **Transactions**: serializable via a database-wide reader/writer lock (readers run in
  parallel, writers are not starved), statement-level atomicity inside `BEGIN … COMMIT`.
- **Query engine**: recursive-descent parser, a planner that pushes predicates down, picks
  indexes for equality / `IN` / range / `BETWEEN` predicates and turns joins on indexed columns
  into index nested-loop joins, and a Volcano-style executor.
- **Network server**: the AtlasDB Wire Protocol (AWP), a multithreaded server, prepared
  statements with `$1` / `?` parameters (no SQL injection by construction), and idle-transaction
  timeouts.
- **Tested**: 63 Catch2 test cases including randomized crash/recovery runs, concurrency stress
  tests, and torn-page repair; a Google Benchmark suite; CI on Linux, macOS and Windows with
  AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer jobs.

## Quick start

Requirements: CMake ≥ 3.20 and a C++20 compiler (GCC 11+, Clang 14+). Catch2 and Google
Benchmark are fetched automatically on the first configure.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

```bash
ctest --test-dir build --output-on-failure
```

| Binary           | Purpose |
|------------------|---------|
| `atlasdb`        | Interactive shell: embedded (`atlasdb DATA_DIR`) or client (`atlasdb --connect HOST:PORT`) |
| `atlasdb-server` | TCP server: `atlasdb-server --data DIR --port 5480 [--idle-txn-timeout SEC] [--no-sync]` |
| `client_example` | Example external application using the C++ client library |
| `atlas_tests`    | Test suite |
| `atlas_bench`    | Benchmarks |

The shell supports `.tables`, `.schema TABLE`, `.read FILE`, `.timer on` and `.quit`, and
also runs scripts via `-f file.sql`, `-c "SQL"` or standard input.

## Using it from an application

C++ ([src/net/client.h](src/net/client.h)):

```cpp
#include "net/client.h"

auto db = atlas::Client::connect("127.0.0.1", 5480, "my-app");

// Values travel separately from the SQL text: no injection possible.
auto r = db.query("SELECT name FROM customers WHERE city = $1 AND age > $2",
                  atlas::params("Córdoba", 30));

// Parsed once, executed many times.
auto transfer = db.prepare("UPDATE accounts SET balance = balance + ? WHERE id = ?");
db.query("BEGIN");
transfer.execute(atlas::params(-10, 1));
transfer.execute(atlas::params(10, 2));
db.query("COMMIT");
```

Python, with no dependencies ([examples/python_client.py](examples/python_client.py)):

```python
db = Connection("127.0.0.1", 5480)
insert = db.prepare("INSERT INTO notes VALUES ($1, $2)")
insert.execute(1, "it's safe; DROP TABLE notes")   # stored as plain text
rows = db.query("SELECT * FROM notes WHERE id >= ?", [1])[0]["rows"]
```

Errors arrive as `atlas::DbError` / `AtlasError` carrying the server's error code
(`constraint_violation`, `syntax_error`, `locked`, …). The engine can also be embedded
directly with `atlas::Database::open(dir)` and `atlas::Session`.

## SQL support

| Area | Supported |
|------|-----------|
| Types | `INTEGER`, `REAL`, `TEXT` / `VARCHAR(n)`, `BOOLEAN`, `NULL` with three-valued logic |
| DDL | `CREATE/DROP TABLE [IF [NOT] EXISTS]`, `CREATE [UNIQUE] INDEX`, `DROP INDEX` |
| Constraints | `PRIMARY KEY`, `UNIQUE`, `NOT NULL`, `DEFAULT`, `CHECK`, `FOREIGN KEY … REFERENCES` (restrict) |
| DML | `INSERT … VALUES` (multi-row), `INSERT … SELECT`, `UPDATE`, `DELETE` |
| Queries | `INNER` / `LEFT` / `CROSS` joins, `WHERE`, `GROUP BY`, `HAVING`, `DISTINCT`, `ORDER BY` (expression, alias or position), `LIMIT … OFFSET` |
| Expressions | arithmetic, `||`, comparisons, `AND/OR/NOT`, `IS [NOT] NULL`, `[NOT] IN`, `[NOT] BETWEEN`, `[NOT] LIKE` |
| Functions | `COUNT`, `SUM`, `AVG`, `MIN`, `MAX` (with `DISTINCT`), `UPPER`, `LOWER`, `LENGTH`, `ABS`, `ROUND`, `SUBSTR`, `COALESCE`, `TYPEOF` |
| Other | `BEGIN` / `COMMIT` / `ROLLBACK`, `EXPLAIN`, `SHOW TABLES`, `DESCRIBE`, `CHECKPOINT`, `$n` / `?` parameters |

## Architecture

```
 SQL ─► Lexer ─► Parser ─► AST ─► Planner ─► operator tree ─► rows
                                     │              │
                                     ▼              ▼
              Database: catalog, constraints,   Table = heap file + B+tree index files
              transactions, WAL, checkpoints            │
                                                        ▼
                                          Pager (buffer pool, checksums) ─► disk
```

| Layer | Code | Notes |
|-------|------|-------|
| Front-end | [src/sql/](src/sql/) | Tokens carry line/column for error messages; expressions serialize back to SQL (used to store `DEFAULT`/`CHECK`) |
| Planner | [planner.cpp](src/engine/planner.cpp) | Star expansion, name resolution, predicate pushdown, index selection, index joins, aggregation rewriting |
| Executor | [operators.cpp](src/engine/operators.cpp) | SeqScan, IndexScan, Filter, NestedLoopJoin, IndexJoin, Aggregate, Project, Distinct, Sort, Limit |
| Storage | [src/storage/](src/storage/) | Slotted pages, buffer pool (no-steal), heap, WAL. Rows over 2 KB are split into chunk records behind a pointer; every physical record change is logged and undone byte for byte, so recovery rebuilds the exact same layout |
| Indexes | [src/index/](src/index/) | B+tree with split / borrow / merge over a pluggable node store (in-memory or paged) |
| Concurrency | [src/txn/](src/txn/) | Session-owned reader/writer lock with writer preference |
| Network | [src/net/](src/net/) | Portable sockets, AWP encoding, server, client |

### Durability and recovery

1. **No-steal + redo-only WAL.** Dirty pages stay in memory until a checkpoint, which only runs
   when no transaction is active, so uncommitted data never reaches the data files. A commit
   appends its redo records plus a `Commit` record to `wal.log`, **releases the lock, then waits
   for durability**: the first waiter fsyncs on behalf of every commit written so far (group
   commit), and reads only return results once the commits they observed are durable.
2. **Atomic checkpoints.** Dirty index nodes are serialized into pages. Every dirty page plus the
   new catalog goes first to `doublewrite.buf` (with a CRC) and is `fsync`ed. Only then are pages
   written in place, the catalog replaced and the WAL truncated.
3. **Opening a database**: a complete doublewrite buffer is replayed, which repairs a checkpoint
   that crashed while writing pages, including torn pages. An incomplete buffer is discarded.
   Then the committed transactions in the WAL that are newer than the checkpoint are redone,
   updating heap and indexes together. A torn WAL tail is detected by CRC and ignored.

On-disk layout of a database directory: `catalog.bin`, `t<id>.tbl` (tables), `i<id>.idx`
(indexes), `wal.log`, and `LOCK` (prevents two processes from opening the same directory).

### Wire protocol

Every message is `[u8 type][u32 length][body]`. A client sends `Hello`, then `Query`, `Prepare`,
`Execute` or `Close`; the server replies with `RowSet` / `Complete` / `Prepared` / `Error` and
always ends with `Ready`, which carries the transaction status. A query with parameters is
pipelined (`Prepare` and `Execute` sent together) for a single round trip. Full specification:
[src/net/protocol.h](src/net/protocol.h).

## Performance

`atlas_bench`, Release build, GCC 15, Windows 11, 8 threads @ 2.9 GHz. Indicative numbers only.

| Benchmark | Result |
|-----------|--------|
| Primary-key lookup, 100k rows (SQL text / prepared statement) | ~11 µs / ~7.5 µs |
| Same lookup on a non-indexed column, 10k rows (full scan) | ~3.3 ms |
| 10,000 inserts in one transaction | ~60 ms |
| Durable autocommit inserts (`fsync`), 1 / 4 / 16 concurrent clients | ~580 / ~1,250 / ~3,860 per second (group commit) |
| Autocommit inserts without `fsync` (`--no-sync`) | ~64k/s |
| `ORDER BY id DESC LIMIT 10` over 100k rows (index order) | ~37 µs |
| `GROUP BY` over 100k rows | ~52 ms |
| Index join 20k × 10k rows + `GROUP BY` | ~33 ms |
| Open a 100k-row database with 2 indexes and run the first query | ~18 ms |
| B+tree: random inserts / lookup in 1M keys | ~9M keys/s / ~250 ns |

## Limitations and roadmap

- One writer at a time (readers run in parallel). Next step: MVCC.
- Single-column indexes and keys; no subqueries, `UNION`, `ALTER TABLE` or views yet.
- Planned: hash joins, cost-based planning with statistics, authentication and TLS for the server.

## License

[MIT](LICENSE)
