# Changelog

All notable changes to AtlasDB are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## [1.0.0] - 2026-11-02

First stable release.

### Storage and durability
- 4 KiB slotted pages with CRC32 checksums, LRU buffer pool, heap files with a lazily built
  free-space map.
- Rows of up to 16 MiB through overflow chains (rows over 2 KB are split into chunk records).
- Redo-only write-ahead log with per-page LSNs and a no-steal buffer policy.
- Group commit: concurrent commits share one `fsync`; results are returned only once every
  commit they observed is durable.
- Atomic checkpoints through a doublewrite buffer, which also repairs pages torn by a crash
  in the middle of a checkpoint.
- Crash recovery that replays committed transactions and keeps indexes in sync.

### Indexes
- B+tree with split, borrow and merge; supports duplicate keys, range scans and reverse scans.
- Persistent: one file per index, nodes loaded lazily and cold nodes evicted under a
  per-index budget, so opening a database does not rebuild anything.

### SQL
- `CREATE/DROP TABLE`, `CREATE [UNIQUE] INDEX`, `DROP INDEX`, `INSERT` (multi-row and
  `INSERT … SELECT`), `SELECT`, `UPDATE`, `DELETE`.
- Inner, left and cross joins; `WHERE`, `GROUP BY`, `HAVING`, `DISTINCT`, `ORDER BY`,
  `LIMIT … OFFSET`; aggregates and scalar functions; three-valued logic.
- Constraints: `PRIMARY KEY`, `UNIQUE`, `NOT NULL`, `DEFAULT`, `CHECK`, `FOREIGN KEY`.
- `$n` / `?` parameters and prepared statements.
- Planner with predicate pushdown, index selection (equality, `IN`, ranges), index
  nested-loop joins, `ORDER BY` served from index order with `LIMIT` pushdown, and top-N sort.
- `EXPLAIN`, `SHOW TABLES`, `DESCRIBE`, `CHECKPOINT`.

### Transactions and concurrency
- Serializable transactions through a reader/writer lock with writer preference; statement-
  level atomicity inside `BEGIN … COMMIT`.

### Server and clients
- AtlasDB Wire Protocol (AWP) v1 with prepared statements and pipelined parameterized queries.
- Multithreaded server with an idle-transaction timeout.
- C++ client library, dependency-free Python client, interactive CLI (embedded or remote).

### Quality
- 73 Catch2 test cases: randomized crash/recovery, concurrency stress, torn-page repair,
  overflow rows and mutation fuzzing.
- CI on Linux (GCC, Clang), macOS and Windows; AddressSanitizer, UndefinedBehaviorSanitizer
  and ThreadSanitizer jobs; coverage-guided libFuzzer job.

### Known limitations
- One writer at a time (no MVCC); single-column indexes and keys; no subqueries, `UNION`,
  `ALTER TABLE` or views; no authentication or TLS in the server.

[1.0.0]: https://github.com/MateoDumas/AtlasDB/releases/tag/v1.0.0
