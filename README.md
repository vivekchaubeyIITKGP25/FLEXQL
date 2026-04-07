# FlexQL

I completed FlexQL as a compact SQL-like database server in C++17. In this submission I implemented the full flow from parsing and execution to durable storage, a multithreaded TCP server, local tests, and benchmark runs.

## What I completed

- `CREATE TABLE` and `CREATE TABLE IF NOT EXISTS`
- single-row and batched `INSERT`
- `SELECT *` and projected `SELECT`
- single-condition `WHERE` with `=`, `!=`, `<`, `>`, `<=`, `>=`
- `INNER JOIN`
- single-column `ORDER BY`
- `DELETE FROM`
- row expiry with `EXPIRES <unix_ms>`
- durable WAL replay on restart
- primary-key hash indexing
- LRU query caching
- multithreaded client/server execution

## Submission contents

This repository now includes the source code, benchmark log, design notes, report source, generated design PDF, and packaged submission assets.

- `README.md` explains the completed work and how I ran it
- `DESIGN.md` contains the implementation notes in first-person form
- `design_doc.tex` contains the report source
- `FLEXQL_DESIGN_DOC.pdf` contains the cleaned report PDF
- `benchmark.out` stores the benchmark output I am submitting
- `diagrams/` contains the architecture and execution figures
- `scripts/package_submission.sh` creates the final zip package

## Build

With the Makefile:

```bash
make -j$(nproc)
```

With CMake:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Both paths build the main binaries in `bin/`.

## Run

Start the server:

```bash
./bin/flexql-server 9000
```

Connect from another terminal:

```bash
./bin/flexql-client 127.0.0.1 9000
```

I also added submission-compatible wrapper names in the packaged zip:

- `bin/flexql_server`
- `bin/flexql_repl`
- `bin/benchmark_flexql`
- `bin/test_driver`

## Tests

I used these test binaries:

```bash
./bin/flexql-test
./bin/flexql-test-all
```

Or both together:

```bash
make test
```

## Benchmark result

I ran the integrated benchmark on April 7, 2026 with:

```bash
./bin/flexql-benchmark 10000000
```

The result I am submitting is also saved in `benchmark.out`.

- rows inserted: `10000000`
- insert elapsed: `9209 ms`
- insert throughput: `1085894 rows/sec`
- multi-client `SELECT` benchmark elapsed: `193780 ms`
- multi-client indexed `WHERE` benchmark elapsed: `26198 ms`
- unit tests: `23/23 passed`

## Project layout

- `src/` contains the server, client, parser, executor, storage, cache, and network code
- `include/` contains the public and internal headers
- `tests/` contains the unit and regression tests
- `benchmarks/` contains the integrated benchmark source
- `scripts/` contains helper and packaging scripts
- `diagrams/` contains submission figures

## Submission package

To generate the final zip file:

```bash
./scripts/package_submission.sh
```

That script builds the project, refreshes the diagrams and report PDF, and writes the cleaned submission archive as `25CS60R64_FLEXQL.zip` in the repository root.
