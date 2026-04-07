# FlexQL Design Notes

Submission package for Roll No. `25CS60R64`

This is a plain description of how I built FlexQL and why I made the tradeoffs I did. I wrote it in the same direct style I used while building the project so the implementation choices stay easy to follow.

## What I was trying to build

I wanted a small SQL-like database that I could reason about end to end. That meant:

- a parser for a limited SQL subset
- an executor that can do basic filtering, joins, and ordering
- disk-backed row storage that stays simple
- durable writes so data survives restart
- a networked server instead of only an in-process API
- enough tests and benchmarks to catch regressions

I was not trying to build a full SQL engine, a query optimizer, or a general-purpose storage engine.

## High-level flow

The flow is straightforward:

1. The server starts.
2. It replays the WAL and rebuilds table files plus any bounded in-memory indexes.
3. It listens for TCP clients.
4. Each client sends SQL using a length-prefixed frame protocol.
5. The server parses the SQL, streams row frames back as the executor emits them, and lets the client abort between rows.
6. Mutating statements become durable only after both the WAL record and its commit marker are flushed.

That last point matters the most to me. I wanted the durability rule to be easy to explain and easy to verify.

## Storage model

I store each table in row-major form on disk. In practice that means each table owns an append-only row file, and each row record stores a `std::vector<std::string>` worth of column values plus an expiration timestamp.

I chose row-major storage because it matches what this project actually does:

- inserts append naturally
- primary-key lookups return full rows
- full-row scans stay simple and streamable
- the code is much easier to maintain than a columnar layout

If I were optimizing for heavy analytics over a few columns, I would not make this choice. For this project, row-major is the better fit.

Each table also stores:

- its name
- schema metadata
- the row-file path on disk
- a row count
- the primary-key column index, if one exists
- a `shared_mutex` for concurrent reads

The table catalog is separate from table storage and is protected by its own lock.

The important change is that the table rows are no longer required to live in RAM all at once. Scans stream rows directly from the table file, which means the stored dataset can be larger than memory.

RAM is only an accelerator in this design. The durable WAL and the disk-backed table files are what preserve data. The in-memory hash index and query cache can always be rebuilt or dropped without losing committed rows.

## Data types

I kept the type system intentionally lightweight.

- `INT` values are stored as strings and compared numerically when needed
- `DECIMAL` values are also stored as strings and parsed on comparison
- `VARCHAR` and `TEXT` stay as strings
- `DATETIME` is stored as text

This is not a strict typed execution engine. It is a pragmatic choice that kept the implementation smaller while still supporting the SQL subset I needed.

## Query support

The parser and executor currently support:

- `CREATE TABLE`
- `CREATE TABLE IF NOT EXISTS`
- `INSERT`
- batched `INSERT`
- `SELECT *`
- projected `SELECT`
- one `WHERE` condition
- `INNER JOIN`
- single-column `ORDER BY`
- `DELETE FROM`

For `WHERE`, I support `=`, `!=`, `<`, `>`, `<=`, and `>=`.

I only support one `WHERE` condition right now. There is no `AND` or `OR`. I left it that way on purpose because I wanted the executor logic to stay clear and predictable.

## Primary-key indexing

For tables with a primary key, I build a bounded in-memory `HashIndex` for the hottest point-lookups. It maps the primary-key value to the row offset inside the table file.

I did this because primary-key lookup is the one indexing case that matters most in this project:

- it is fast on average
- it is simple to rebuild on restart
- it keeps the executor logic uncomplicated

I keep that in-memory index bounded. If a table grows past the configured resident-index limit, FlexQL drops only the resident hash map and keeps using a disk-backed primary-key hash file as the overflow path. That keeps the dataset size from being capped by index RAM without turning every large-table point lookup or duplicate-key check back into a full-table scan.

The execution rule is simple:

- if a `WHERE` clause hits the primary key, I try the in-memory hash index first
- if that resident index has spilled, I use the disk-backed primary-key hash file
- otherwise I scan the table

That keeps the common path fast without pretending I have a full optimizer.

## Cache

I use an LRU cache for `SELECT` results. The cache key is built from the table name plus the relevant parts of the query such as projected columns, `WHERE`, `JOIN`, and `ORDER BY`.

I chose LRU because it solves the repeated-query case with very little machinery:

- `get` is cheap
- `put` is cheap
- eviction behavior is easy to understand
- I do not need frequency counters or more complicated policies

On writes, I invalidate cache entries for the touched table. I also keep expiration timestamps with cached rows, so a cache hit still re-checks TTL before returning rows.

The cache is bounded by both entry count and approximate bytes. Large result sets can stream through the executor, but they are not retained in cache once they cross the configured byte budget.

## Expiration

Rows can expire through this syntax:

```sql
INSERT INTO ORDERS VALUES (1,'Widget',9.99) EXPIRES 1735689600000;
```

Each row has an `expires_at` field in Unix milliseconds.

I enforce expiration lazily:

- expired rows are skipped during reads
- cached rows are filtered again on cache hits
- expired rows do not block a new row from reusing the same primary key

I preferred lazy expiration because it kept the write path simpler. If I want eager cleanup later, there is room to add a background compaction pass.

## Durability and recovery

This is the part I cared about most.

FlexQL uses an append-only durable log, `data/flexql.wal` by default. I treat that file as the primary source of truth. The table files are a disk-backed working set that the server rebuilds from the WAL on startup.

The rule for mutations is:

1. validate the statement
2. append a prepared WAL record
3. flush it
4. apply the change locally
5. append a commit marker
6. flush it
7. acknowledge success

If local apply or the commit-marker flush fails, FlexQL rolls the local change back and returns an error. On restart, replay ignores any prepared WAL record that does not have a matching durable commit marker. If a tail record is truncated, replay ignores that incomplete suffix and keeps the earlier valid records.

I chose this design because it gave me the cleanest durability story for the amount of code I wanted to write. It is not the fastest possible write path, but it is easy to reason about when something crashes.

To keep large batched inserts practical without weakening durability, I reuse each table's append file descriptor across inserts instead of reopening the table file for every row. I also maintain a derived disk-backed primary-key hash file so duplicate-key checks and point lookups stay fast even after the resident in-memory index spills. Those optimizations do not change the commit rule: replay only treats WAL entries with a matching commit marker as committed, and the row data is still written to persistent storage rather than buffered only in RAM.

This is an intentional middle ground. I did not switch to an in-memory write buffer that flushes later, because that would blur the durability story. Instead, I reduced repeated syscall overhead on the same persistent file while keeping the durable-log-first contract intact.

## Concurrency model

The server uses a thread pool sized as:

```text
max(4, hardware_concurrency * 2)
```

Each client connection is handed to the pool. The `Executor` instance is shared.

The locking approach is conservative:

- the catalog uses a shared mutex
- each table uses a shared mutex
- the hash index has its own lock
- the cache has its own mutex
- the durable log has its own mutex
- mutations are serialized through `Executor::mutationMutex_`

This means reads can proceed in parallel, but writes are deliberately ordered. I accepted that tradeoff because correctness and durability were more important to me than squeezing out the last bit of write concurrency.

One practical result of this design is that FlexQL can now store tables that are larger than RAM. The main remaining pressure points are queries that deliberately materialize large intermediate results, especially `ORDER BY` and large joins, because those operations still need to gather rows before they can sort them.

I also tried not to optimize only for the provided benchmark script. Batched inserts help because they reduce network round trips and amortize WAL work, but the storage layout, primary-key indexing, and query cache are all useful for general client workloads too. The benchmark is a sanity check, not the whole design target.

## Network protocol

The wire protocol is length-prefixed TCP. Each frame is one length-prefixed payload:

```text
Request:  [4-byte big-endian length][frame bytes]
Response: [4-byte big-endian length][frame bytes]
```

The client sends either:

```text
QUERY\n<sql>
ABORT
EXIT
```

The server responds with streamed frames:

```text
HEADER\n<tab-separated column names>
ROW\n<tab-separated row values>
END
ABORTED
ERR\n<message>
```

Rows and headers are tab-separated. The important point is that the server no longer builds one giant response string first. It sends rows incrementally, and the client can send `ABORT` after any callback invocation. One caveat is that `ORDER BY` and some joins still materialize intermediate rows inside the executor before they can be sorted.

## Build and run

I kept the build simple.

With CMake:

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cd ..
```

With the Makefile:

```bash
make -j$(nproc)
```

Run the server:

```bash
./bin/flexql-server 9000
```

Connect with the REPL:

```bash
./bin/flexql-client 127.0.0.1 9000
```

Use a different WAL file if I want a clean run:

```bash
./bin/flexql-server 9000 /tmp/flexql-demo.wal
```

Use a separate storage root if I want the table files somewhere else:

```bash
FLEXQL_STORAGE_ROOT=/tmp/flexql-demo.tables ./bin/flexql-server 9000 /tmp/flexql-demo.wal
```

## Tests

I currently use two local test binaries:

- `./bin/flexql-test`
- `./bin/flexql-test-all`

Or both together:

```bash
make test
```

The test coverage is aimed at the main failure-prone parts of the system:

- parser behavior
- executor behavior
- joins and ordering
- cache invalidation
- TTL handling
- durable log replay
- snapshot round-trip
- concurrency smoke checks

## Benchmarking

There are two benchmark paths:

```bash
python3 scripts/benchmark.py --host 127.0.0.1 --port 9000 --rows 1000000
./bin/flexql-benchmark --unit-test
./bin/flexql-benchmark 10000000
```

For a clean benchmark run, I start the server with a fresh WAL:

```bash
./bin/flexql-server 9000 /tmp/flexql-benchmark.wal
```

On April 7, 2026, I ran the integrated benchmark with `10000000` rows and recorded the result in `benchmark.out`. The main numbers from that run were:

- insert elapsed: `9,209 ms`
- insert throughput: `1,085,894 rows/sec`
- multi-client full-table `SELECT`: `193,780 ms`
- multi-client primary-key `WHERE`: `26,198 ms`
- unit tests: `23/23 passed`

I treat that as a machine-specific measurement, not a universal claim.

## What I think is good about this design

- I can explain the whole system without hand-waving.
- Recovery behavior is simple.
- Primary-key lookups are fast.
- The code stays small enough to debug.
- The test surface covers the parts most likely to break.

## Where I would improve it next

- better query planning instead of simple rule-based execution
- secondary indexes
- richer `WHERE` support with `AND` and `OR`
- better compaction so replay cost stays lower over time
- more realistic multi-client benchmarks

## Folder layout

```text
flexql/
|-- bin/
|-- include/
|-- src/
|-- tests/
|-- benchmarks/
|-- scripts/
|-- CMakeLists.txt
|-- Makefile
|-- README.md
`-- DESIGN.md
```

That layout mirrors the way I think about the project: public headers, implementation, tests, and a small set of helper scripts.
