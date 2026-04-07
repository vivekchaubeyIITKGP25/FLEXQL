#!/usr/bin/env python3
"""
FlexQL Benchmark Script
Measures INSERT throughput and SELECT latency.
Usage:
    python3 scripts/benchmark.py --host 127.0.0.1 --port 9000 --rows 1000000

"""

import socket
import struct
import time
import argparse
import random
import string
import sys
import os

# ─── Protocol helpers ────────────────────────────────────────────────────────

def send_msg(sock, msg: str):
    data = msg.encode('utf-8')
    header = struct.pack('>I', len(data))
    sock.sendall(header + data)

def recv_msg(sock) -> str:
    raw_len = recvall(sock, 4)
    if not raw_len:
        return None
    msg_len = struct.unpack('>I', raw_len)[0]
    data = recvall(sock, msg_len)
    return data.decode('utf-8') if data else None

def recvall(sock, n: int) -> bytes:
    buf = b''
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf

# ─── Benchmark helpers ───────────────────────────────────────────────────────

def rand_str(n=8):
    return ''.join(random.choices(string.ascii_letters, k=n))

def exec_query(sock, sql: str) -> str:
    send_msg(sock, sql)
    return recv_msg(sock)

def is_error_response(resp: str) -> bool:
    return resp is None or resp.startswith("ERR")

def error_text(resp: str) -> str:
    if resp is None:
        return "connection closed"
    lines = resp.splitlines()
    if lines and lines[0] == "ERR":
        return '\n'.join(lines[1:]).strip() or "unknown error"
    return resp.strip() or "unknown error"

def require_ok(resp: str, label: str) -> str:
    if is_error_response(resp):
        raise RuntimeError(f"{label}: {error_text(resp)}")
    return resp

def count_result_rows(resp: str) -> int:
    lines = (resp or "").splitlines()
    if not lines or lines[0] != "OK":
        raise RuntimeError(f"Unexpected response: {error_text(resp)}")
    if lines[-1] != "END":
        raise RuntimeError("Malformed response: missing END marker")
    if len(lines) <= 2:
        return 0
    return len(lines) - 3

# ─── Main benchmark ──────────────────────────────────────────────────────────

def run_benchmark(host, port, num_rows):
    if num_rows <= 0:
        raise ValueError("--rows must be a positive integer")

    print(f"FlexQL Benchmark — {num_rows:,} rows")
    print(f"Connecting to {host}:{port}...")

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sock.connect((host, port))
    print("Connected.\n")

    # ── Create table ──────────────────────────────────────────────────────────
    create_sql = (
        "CREATE TABLE IF NOT EXISTS BENCH ("
        "ID INT PRIMARY KEY NOT NULL, "
        "FIRST_NAME VARCHAR NOT NULL, "
        "LAST_NAME VARCHAR NOT NULL, "
        "EMAIL VARCHAR NOT NULL, "
        "SCORE DECIMAL NOT NULL"
        ");"
    )
    require_ok(exec_query(sock, create_sql), "CREATE TABLE BENCH")
    require_ok(exec_query(sock, "DELETE FROM BENCH;"), "DELETE FROM BENCH")
    print("[OK] Table BENCH ready.")

    # ── INSERT benchmark ──────────────────────────────────────────────────────
    print(f"\n--- INSERT {num_rows:,} rows ---")
    batch_size = 10000
    total_inserted = 0
    insert_start = time.perf_counter()

    pending_ids = []
    for i in range(num_rows):
        fn   = rand_str(6)
        ln   = rand_str(8)
        mail = f"{fn.lower()}.{ln.lower()}@example.com"
        score = round(random.uniform(0, 100), 2)
        sql = f"INSERT INTO BENCH VALUES ({i},'{fn}','{ln}','{mail}',{score});"
        send_msg(sock, sql)
        pending_ids.append(i)

        if len(pending_ids) == batch_size:
            # Drain responses in batch for throughput
            for row_id in pending_ids:
                require_ok(recv_msg(sock), f"INSERT row {row_id}")
            total_inserted += len(pending_ids)
            pending_ids.clear()
            elapsed = time.perf_counter() - insert_start
            rate = total_inserted / elapsed
            pct  = total_inserted / num_rows * 100
            print(f"  {total_inserted:>10,} rows  {elapsed:6.1f}s  {rate:>10,.0f} rows/sec  ({pct:.0f}%)")

    # Drain remainder
    for row_id in pending_ids:
        require_ok(recv_msg(sock), f"INSERT row {row_id}")
    total_inserted += len(pending_ids)

    insert_end  = time.perf_counter()
    insert_time = insert_end - insert_start
    insert_rate = num_rows / insert_time

    print(f"\n[RESULT] INSERT: {num_rows:,} rows in {insert_time:.3f}s")
    print(f"         Throughput: {insert_rate:,.0f} rows/sec")

    # ── SELECT * (full scan) ──────────────────────────────────────────────────
    print(f"\n--- SELECT * FROM BENCH (full scan) ---")
    t0 = time.perf_counter()
    resp = require_ok(exec_query(sock, "SELECT * FROM BENCH;"), "SELECT * FROM BENCH")
    t1 = time.perf_counter()
    row_count = count_result_rows(resp)
    print(f"[RESULT] Full scan: {t1-t0:.3f}s  ({row_count:,} rows received)")

    # ── SELECT with PK WHERE (index lookup) ──────────────────────────────────
    print(f"\n--- SELECT with PK lookup (1000 random queries) ---")
    ids = random.sample(range(num_rows), min(1000, num_rows))
    t0 = time.perf_counter()
    hits = 0
    for pk in ids:
        send_msg(sock, f"SELECT * FROM BENCH WHERE ID = {pk};")
        r = require_ok(recv_msg(sock), f"SELECT ID = {pk}")
        if count_result_rows(r) == 1:
            hits += 1
    t1 = time.perf_counter()
    lookup_count = len(ids)
    print(f"[RESULT] {lookup_count} PK lookups in {t1-t0:.3f}s  avg {(t1-t0)/lookup_count*1000:.2f}ms each  ({hits}/{lookup_count} hits)")

    # ── SELECT cached (repeat same query) ────────────────────────────────────
    print(f"\n--- SELECT cached (same query repeated 100x) ---")
    cached_id = min(42, num_rows - 1)
    sql_c = f"SELECT * FROM BENCH WHERE ID = {cached_id};"
    require_ok(exec_query(sock, sql_c), f"Warm cache for ID = {cached_id}")
    t0 = time.perf_counter()
    for _ in range(100):
        require_ok(exec_query(sock, sql_c), f"Cached SELECT ID = {cached_id}")
    t1 = time.perf_counter()
    print(f"[RESULT] 100 cached lookups in {t1-t0:.3f}s  avg {(t1-t0)/100*1000:.3f}ms each")

    # ── Memory usage ──────────────────────────────────────────────────────────
    try:
        with open('/proc/self/status') as f:
            for line in f:
                if line.startswith('VmRSS'):
                    print(f"\n[MEM] Client RSS: {line.split()[1]} kB")
    except Exception:
        pass

    # Check server memory if running locally
    try:
        import subprocess
        out = subprocess.check_output(
            ['pgrep', '-a', 'flexql-server'], text=True)
        pid = out.split()[0]
        with open(f'/proc/{pid}/status') as f:
            for line in f:
                if line.startswith('VmRSS'):
                    print(f"[MEM] Server RSS: {line.split()[1]} kB")
    except Exception:
        pass

    sock.close()
    print("\n[DONE] Benchmark complete.")

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='FlexQL Benchmark')
    parser.add_argument('--host', default='127.0.0.1')
    parser.add_argument('--port', type=int, default=9000)
    parser.add_argument('--rows', type=int, default=100000)
    args = parser.parse_args()
    try:
        run_benchmark(args.host, args.port, args.rows)
    except Exception as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        sys.exit(1)
