#!/bin/bash
# FlexQL build + run helper
# Usage:
#   ./scripts/run.sh build          - Compile everything
#   ./scripts/run.sh server [port]  - Start the server (default port 9000)
#   ./scripts/run.sh client [host] [port]  - Start interactive REPL
#   ./scripts/run.sh test           - Run the test suite
#   ./scripts/run.sh bench [rows]   - Run benchmark (server must be running)
#

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."
cd "$ROOT"

cmd="${1:-build}"

build_cmake() {
    echo "[build] Using CMake..."
    mkdir -p build
    cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release -G "Unix Makefiles" 2>/dev/null || cmake ..
    make -j"$(nproc)"
    cd ..
    echo "[build] Done. Binaries in bin/"
}

build_make() {
    echo "[build] Using Makefile..."
    make -j"$(nproc)"
    echo "[build] Done. Binaries in bin/"
}

case "$cmd" in
    build)
        if command -v cmake &>/dev/null; then
            build_cmake
        else
            build_make
        fi
        ;;

    server)
        port="${2:-9000}"
        if [ ! -f bin/flexql-server ]; then
            echo "[run] Server not built. Building first..."
            $0 build
        fi
        echo "[run] Starting FlexQL server on port $port ..."
        ./bin/flexql-server "$port"
        ;;

    client)
        host="${2:-127.0.0.1}"
        port="${3:-9000}"
        if [ ! -f bin/flexql-client ]; then
            echo "[run] Client not built. Building first..."
            $0 build
        fi
        ./bin/flexql-client "$host" "$port"
        ;;

    test)
        if [ ! -f bin/flexql-test ]; then
            $0 build
        fi
        echo "[test] Running FlexQL test suite..."
        ./bin/flexql-test
        ;;

    bench)
        rows="${2:-1000000}"
        echo "[bench] Running benchmark with $rows rows..."
        python3 scripts/benchmark.py --rows "$rows"
        ;;

    clean)
        make clean
        rm -rf build
        ;;

    *)
        echo "Unknown command: $cmd"
        echo "Usage: $0 {build|server|client|test|bench|clean}"
        exit 1
        ;;
esac
