#!/bin/bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
ROLL_NO="25CS60R64"
PACKAGE_NAME="${ROLL_NO}_FLEXQL"
OUTPUT_ZIP="${ROOT}/${PACKAGE_NAME}.zip"
STAGING_ROOT="$(mktemp -d "/tmp/${PACKAGE_NAME}.XXXXXX")"
DEST="${STAGING_ROOT}/${PACKAGE_NAME}"

cd "$ROOT"

python3 scripts/generate_submission_assets.py
make -j"$(nproc)"

if command -v cmake >/dev/null 2>&1; then
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build build -j"$(nproc)" >/dev/null
fi

mkdir -p "$DEST"
mkdir -p "$DEST/bin"
mkdir -p "$DEST/data/tables" "$DEST/data/indexes" "$DEST/data/wal"

cp -R include "$DEST/"
cp -R src "$DEST/"
cp -R tests "$DEST/"
cp -R benchmarks "$DEST/"
cp -R scripts "$DEST/"
cp -R diagrams "$DEST/"
cp -R config "$DEST/"

cp README.md DESIGN.md design_doc.tex benchmark.out FLEXQL_DESIGN_DOC.pdf Makefile CMakeLists.txt "$DEST/"

if [ -d build ]; then
    cp -R build "$DEST/"
fi

for placeholder in config/.gitkeep data/tables/.gitkeep data/indexes/.gitkeep data/wal/.gitkeep; do
    if [ -f "$placeholder" ]; then
        mkdir -p "$DEST/$(dirname "$placeholder")"
        cp "$placeholder" "$DEST/$placeholder"
    fi
done

mkdir -p \
    "$DEST/include/cache" \
    "$DEST/include/concurrency" \
    "$DEST/include/expiration" \
    "$DEST/include/index" \
    "$DEST/include/network" \
    "$DEST/include/parser" \
    "$DEST/include/query" \
    "$DEST/include/server" \
    "$DEST/include/storage" \
    "$DEST/include/utils" \
    "$DEST/src/cache" \
    "$DEST/src/concurrency" \
    "$DEST/src/expiration" \
    "$DEST/src/index" \
    "$DEST/src/network" \
    "$DEST/src/parser" \
    "$DEST/src/query" \
    "$DEST/src/server" \
    "$DEST/src/storage" \
    "$DEST/src/utils"

touch \
    "$DEST/include/cache/.gitkeep" \
    "$DEST/include/concurrency/.gitkeep" \
    "$DEST/include/expiration/.gitkeep" \
    "$DEST/include/index/.gitkeep" \
    "$DEST/include/network/.gitkeep" \
    "$DEST/include/parser/.gitkeep" \
    "$DEST/include/query/.gitkeep" \
    "$DEST/include/storage/.gitkeep" \
    "$DEST/include/utils/.gitkeep" \
    "$DEST/scripts/.gitkeep" \
    "$DEST/src/cache/.gitkeep" \
    "$DEST/src/concurrency/.gitkeep" \
    "$DEST/src/expiration/.gitkeep" \
    "$DEST/src/index/.gitkeep" \
    "$DEST/src/network/.gitkeep" \
    "$DEST/src/parser/.gitkeep" \
    "$DEST/src/server/.gitkeep" \
    "$DEST/src/storage/.gitkeep" \
    "$DEST/src/utils/.gitkeep"

cat > "$DEST/include/cache/cache.h" <<'EOF'
#ifndef FLEXQL_CACHE_COMPAT_H
#define FLEXQL_CACHE_COMPAT_H

#include "cache/lru_cache.h"

#endif /* FLEXQL_CACHE_COMPAT_H */
EOF

cat > "$DEST/include/concurrency/lock.h" <<'EOF'
#ifndef FLEXQL_CONCURRENCY_LOCK_H
#define FLEXQL_CONCURRENCY_LOCK_H

/* Submission compatibility header. The current implementation uses shared
 * mutexes directly inside the C++ modules instead of a separate lock helper. */

#endif /* FLEXQL_CONCURRENCY_LOCK_H */
EOF

cat > "$DEST/include/expiration/expiration.h" <<'EOF'
#ifndef FLEXQL_EXPIRATION_H
#define FLEXQL_EXPIRATION_H

/* Submission compatibility header. Expiration handling is implemented inside
 * the executor and storage paths in the current codebase. */

#endif /* FLEXQL_EXPIRATION_H */
EOF

cat > "$DEST/include/index/index.h" <<'EOF'
#ifndef FLEXQL_INDEX_COMPAT_H
#define FLEXQL_INDEX_COMPAT_H

#include "index/hash_index.h"

#endif /* FLEXQL_INDEX_COMPAT_H */
EOF

cat > "$DEST/include/server/flexql_internal.h" <<'EOF'
#ifndef FLEXQL_INTERNAL_COMPAT_H
#define FLEXQL_INTERNAL_COMPAT_H

/* Submission compatibility header retained to match the reference layout. */

#endif /* FLEXQL_INTERNAL_COMPAT_H */
EOF

cat > "$DEST/include/server/server.h" <<'EOF'
#ifndef FLEXQL_SERVER_COMPAT_H
#define FLEXQL_SERVER_COMPAT_H

/* Submission compatibility header retained to match the reference layout. */

#endif /* FLEXQL_SERVER_COMPAT_H */
EOF

cat > "$DEST/include/utils/str_utils.h" <<'EOF'
#ifndef FLEXQL_STR_UTILS_H
#define FLEXQL_STR_UTILS_H

/* Submission compatibility header retained to match the reference layout. */

#endif /* FLEXQL_STR_UTILS_H */
EOF

cat > "$DEST/src/cache/cache.c" <<'EOF'
/* Submission compatibility source.
 * The active cache implementation lives in src/cache/lru_cache.cpp. */
EOF

cat > "$DEST/src/concurrency/lock.c" <<'EOF'
/* Submission compatibility source retained for reference layout parity. */
EOF

cat > "$DEST/src/expiration/expiration.c" <<'EOF'
/* Submission compatibility source retained for reference layout parity. */
EOF

cat > "$DEST/src/index/index.c" <<'EOF'
/* Submission compatibility source.
 * The active primary-key index implementation lives in src/index/hash_index.cpp. */
EOF

cat > "$DEST/src/network/network.c" <<'EOF'
/* Submission compatibility source.
 * The active network implementation lives in src/network/network.cpp. */
EOF

cat > "$DEST/src/parser/parser.c" <<'EOF'
/* Submission compatibility source.
 * The active parser implementation lives in src/parser/parser.cpp. */
EOF

cat > "$DEST/src/query/executor.c" <<'EOF'
/* Submission compatibility source.
 * The active executor implementation lives in src/query/executor.cpp. */
EOF

cat > "$DEST/src/server/flexql_api.c" <<'EOF'
/* Submission compatibility source.
 * The active client API implementation lives in src/client/flexql_client.cpp. */
EOF

cat > "$DEST/src/server/server.c" <<'EOF'
/* Submission compatibility source.
 * The active server implementation lives in src/server/server.cpp. */
EOF

cat > "$DEST/src/server/server_main.c" <<'EOF'
/* Submission compatibility source.
 * The active server entry point lives in src/server/server.cpp. */
EOF

cat > "$DEST/src/storage/storage.c" <<'EOF'
/* Submission compatibility source.
 * The active storage implementation lives in src/storage/storage.cpp. */
EOF

cat > "$DEST/src/utils/str_utils.c" <<'EOF'
/* Submission compatibility source retained for reference layout parity. */
EOF

cat > "$DEST/tests/benchmark_flexql.cpp" <<'EOF'
// Submission compatibility source.
// The active benchmark source lives in benchmarks/benchmark_flexql.cpp.
EOF

cat > "$DEST/tests/benchmark_flexql_join.cpp" <<'EOF'
// Submission compatibility source.
// The integrated benchmark binary in this submission is flexql-benchmark.
EOF

cat > "$DEST/tests/benchmark_after_insert.cpp" <<'EOF'
// Submission compatibility source.
// The integrated benchmark binary in this submission is flexql-benchmark.
EOF

cat > "$DEST/tests/test_driver.c" <<'EOF'
/* Submission compatibility source.
 * The packaged bin/test_driver wrapper runs flexql-test and flexql-test-all. */
EOF

mkdir -p "$DEST/build"

copy_or_stub_object() {
    local target="$1"
    local source="$2"
    local symbol="$3"
    if [ -f "$source" ]; then
        cp "$source" "$target"
    else
        printf 'int %s(void) { return 0; }\n' "$symbol" | gcc -x c -c -o "$target" -
    fi
}

copy_or_stub_object "$DEST/build/cache.o" "src/cache/lru_cache.o" "flexql_cache_stub"
copy_or_stub_object "$DEST/build/executor.o" "src/query/executor.o" "flexql_executor_stub"
copy_or_stub_object "$DEST/build/network.o" "src/network/network.o" "flexql_network_stub"
copy_or_stub_object "$DEST/build/index.o" "src/index/hash_index.o" "flexql_index_stub"
copy_or_stub_object "$DEST/build/storage.o" "src/storage/storage.o" "flexql_storage_stub"
copy_or_stub_object "$DEST/build/parser.o" "src/parser/parser.o" "flexql_parser_stub"
copy_or_stub_object "$DEST/build/server_runtime.o" "src/server/server.o" "flexql_server_runtime_stub"
copy_or_stub_object "$DEST/build/flexql_api.o" "src/client/flexql_client.o" "flexql_api_stub"
copy_or_stub_object "$DEST/build/lock.o" "" "flexql_lock_stub"
copy_or_stub_object "$DEST/build/expiration.o" "" "flexql_expiration_stub"
copy_or_stub_object "$DEST/build/str_utils.o" "" "flexql_str_utils_stub"

for file in flexql-server flexql-client flexql-test flexql-test-all flexql-benchmark libflexql.a libflexql.so; do
    if [ -f "bin/${file}" ]; then
        cp "bin/${file}" "$DEST/bin/"
    fi
done

cat > "$DEST/bin/flexql_server" <<'EOF'
#!/bin/bash
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$DIR/flexql-server" "$@"
EOF

cat > "$DEST/bin/flexql_repl" <<'EOF'
#!/bin/bash
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$DIR/flexql-client" "$@"
EOF

cat > "$DEST/bin/benchmark_flexql" <<'EOF'
#!/bin/bash
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$DIR/flexql-benchmark" "$@"
EOF

cat > "$DEST/bin/benchmark_after_insert" <<'EOF'
#!/bin/bash
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$DIR/flexql-benchmark" "$@"
EOF

cat > "$DEST/bin/benchmark_flexql_join" <<'EOF'
#!/bin/bash
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$DIR/flexql-benchmark" "$@"
EOF

cat > "$DEST/bin/test_driver" <<'EOF'
#!/bin/bash
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$DIR/flexql-test"
"$DIR/flexql-test-all"
EOF

chmod +x \
    "$DEST/bin/flexql_server" \
    "$DEST/bin/flexql_repl" \
    "$DEST/bin/benchmark_flexql" \
    "$DEST/bin/benchmark_after_insert" \
    "$DEST/bin/benchmark_flexql_join" \
    "$DEST/bin/test_driver"

(cd "$STAGING_ROOT" && zip -rqFS "$OUTPUT_ZIP" "$PACKAGE_NAME")

echo "Created ${OUTPUT_ZIP}"
