/*
 * FlexQL: Test Suite
 * Tests storage, indexing, cache, parser, and executor in-process
 * (no network needed — tests the engine directly).
 *
 */

#include "flexql.h"
#include "storage.h"
#include "hash_index.h"
#include "lru_cache.h"
#include "parser.h"
#include "durable_log.h"
#include "persistence.h"
#include "executor.h"
#include "network.h"
#include "protocol.h"
#include "session.h"

#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

/* ─── Mini test framework ─────────────────────────────────────────────────── */

static int g_pass = 0, g_fail = 0;

#define TEST(name) void test_##name()
#define RUN(name)  do { \
    std::cout << "  [ RUN ] " #name "\n"; \
    try { test_##name(); std::cout << "  [  OK ] " #name "\n"; ++g_pass; } \
    catch (std::exception &e) { \
        std::cout << "  [FAIL ] " #name ": " << e.what() << "\n"; ++g_fail; } \
    catch (...) { \
        std::cout << "  [FAIL ] " #name ": unknown exception\n"; ++g_fail; } \
} while(0)

#define ASSERT(cond) do { if (!(cond)) throw std::runtime_error("ASSERT failed: " #cond); } while(0)
#define ASSERT_EQ(a,b) do { if ((a) != (b)) throw std::runtime_error( \
    std::string("ASSERT_EQ failed: ") + std::to_string(a) + " != " + std::to_string(b)); } while(0)
#define ASSERT_STR(a,b) do { if (std::string(a) != std::string(b)) throw std::runtime_error( \
    std::string("ASSERT_STR failed: [") + (a) + "] != [" + (b) + "]"); } while(0)

/* ─── Helper: build a simple executor ────────────────────────────────────────*/

struct Env {
    StorageEngine storage;
    LRUCache      cache{256};
    Executor      executor{storage, cache};
    Parser        parser;

    int exec(const std::string &sql, flexql_callback cb = nullptr, void *arg = nullptr) {
        auto q = parser.parse(sql);
        if (!q.error.empty()) throw std::runtime_error("Parse error: " + q.error);
        std::string err;
        int rc = executor.execute(q, cb, arg, err);
        if (rc != FLEXQL_OK) throw std::runtime_error("Exec error: " + err);
        return rc;
    }
};

static void exec_sql(Executor &executor, Parser &parser, const std::string &sql) {
    auto q = parser.parse(sql);
    if (!q.error.empty()) {
        throw std::runtime_error("Parse error: " + q.error);
    }
    std::string err;
    const int rc = executor.execute(q, nullptr, nullptr, err);
    if (rc != FLEXQL_OK) {
        throw std::runtime_error("Exec error: " + err);
    }
}

static std::vector<std::string> collect_rows(Executor &executor,
                                             Parser &parser,
                                             const std::string &sql) {
    struct Ctx {
        std::vector<std::string> rows;
    } ctx;

    auto cb = [](void *data, int ncols, char **vals, char **) -> int {
        auto *ctx = static_cast<Ctx *>(data);
        std::string row;
        for (int i = 0; i < ncols; ++i) {
            if (i > 0) {
                row += "|";
            }
            row += (vals[i] != nullptr) ? vals[i] : "NULL";
        }
        ctx->rows.push_back(std::move(row));
        return 0;
    };

    auto q = parser.parse(sql);
    if (!q.error.empty()) {
        throw std::runtime_error("Parse error: " + q.error);
    }
    std::string err;
    const int rc = executor.execute(q, cb, &ctx, err);
    if (rc != FLEXQL_OK) {
        throw std::runtime_error("Exec error: " + err);
    }
    return ctx.rows;
}

static void rebuild_all_indexes(StorageEngine &storage, Executor &executor) {
    for (const std::string &tableName : storage.listTables()) {
        executor.rebuildIndex(tableName);
    }
}

static void open_socket_pair(int fds[2]) {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        throw std::runtime_error("Failed to create socketpair");
    }
}

static void set_socket_timeout(int fd, int milliseconds) {
    timeval tv{};
    tv.tv_sec = milliseconds / 1000;
    tv.tv_usec = (milliseconds % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

int flexql_open_fd_for_test(int fd, FlexQL **db);

/* ─── Tests ───────────────────────────────────────────────────────────────── */

TEST(storage_create) {
    Env e;
    e.exec("CREATE TABLE T1 (ID INT PRIMARY KEY NOT NULL, NAME VARCHAR NOT NULL)");
    ASSERT(e.storage.tableExists("T1"));
    ASSERT(!e.storage.tableExists("NOPE"));
}

TEST(storage_insert_select) {
    Env e;
    e.exec("CREATE TABLE STUDENT (ID INT PRIMARY KEY NOT NULL, NAME TEXT NOT NULL)");
    e.exec("INSERT INTO STUDENT VALUES (1,'Alice')");
    e.exec("INSERT INTO STUDENT VALUES (2,'Bob')");

    int rowCount = 0;
    auto cb = [](void *d, int ncols, char **vals, char **names) -> int {
        (void)ncols;
        (void)vals;
        (void)names;
        (*(int*)d)++;
        return 0;
    };
    auto q = e.parser.parse("SELECT * FROM STUDENT");
    std::string err;
    e.executor.execute(q, cb, &rowCount, err);
    ASSERT_EQ(rowCount, 2);
}

TEST(where_equality) {
    Env e;
    e.exec("CREATE TABLE EMP (ID INT PRIMARY KEY, NAME TEXT)");
    e.exec("INSERT INTO EMP VALUES (1,'Alice')");
    e.exec("INSERT INTO EMP VALUES (2,'Bob')");
    e.exec("INSERT INTO EMP VALUES (3,'Charlie')");

    struct Ctx { std::vector<std::string> names; };
    Ctx ctx;
    auto cb = [](void *d, int ncols, char **vals, char **names) -> int {
        auto *c = (Ctx*)d;
        for (int i = 0; i < ncols; ++i)
            if (std::string(names[i]) == "NAME") c->names.push_back(vals[i]);
        return 0;
    };
    auto q = e.parser.parse("SELECT * FROM EMP WHERE ID = 2");
    std::string err;
    e.executor.execute(q, cb, &ctx, err);
    ASSERT_EQ((int)ctx.names.size(), 1);
    ASSERT_STR(ctx.names[0], "Bob");
}

TEST(hash_index) {
    HashIndex idx;
    idx.insert("42", 100);
    idx.insert("7",  200);
    ASSERT_EQ(idx.lookup("42"), 100u);
    ASSERT_EQ(idx.lookup("7"),  200u);
    ASSERT_EQ(idx.lookup("99"), SIZE_MAX);
    idx.remove("42");
    ASSERT_EQ(idx.lookup("42"), SIZE_MAX);
}

TEST(lru_cache) {
    LRUCache cache(3);
    CachedResult r1, r2, r3, r4;
    r1.columnNames = {"A"}; r1.rows = {{"1"}};
    r2.columnNames = {"B"}; r2.rows = {{"2"}};
    r3.columnNames = {"C"}; r3.rows = {{"3"}};
    r4.columnNames = {"D"}; r4.rows = {{"4"}};

    cache.put("k1", r1);
    cache.put("k2", r2);
    cache.put("k3", r3);
    ASSERT_EQ(cache.size(), 3u);

    CachedResult out;
    ASSERT(cache.get("k1", out));        // k1 is now MRU
    cache.put("k4", r4);                 // evicts LRU (k2)
    ASSERT(!cache.get("k2", out));       // k2 evicted
    ASSERT(cache.get("k1", out));        // k1 still in
    ASSERT(cache.get("k3", out));        // k3 still in
    ASSERT(cache.get("k4", out));        // k4 in
}

TEST(parser_create) {
    Parser p;
    auto q = p.parse("CREATE TABLE FOO (ID INT PRIMARY KEY NOT NULL, VAL DECIMAL);");
    ASSERT(q.error.empty());
    ASSERT_STR(q.tableName, "FOO");
    ASSERT_EQ((int)q.columns.size(), 2);
    ASSERT(q.columns[0].primaryKey);
    ASSERT(q.columns[0].notNull);
}

TEST(parser_insert) {
    Parser p;
    auto q = p.parse("INSERT INTO ORDERS VALUES (10,'Widget',99.99);");
    ASSERT(q.error.empty());
    ASSERT_STR(q.tableName, "ORDERS");
    ASSERT_EQ((int)q.insertValues.size(), 3);
    ASSERT_STR(q.insertValues[1], "Widget");
}

TEST(parser_select_where) {
    Parser p;
    auto q = p.parse("SELECT ID, NAME FROM STUDENT WHERE ID = 5;");
    ASSERT(q.error.empty());
    ASSERT_EQ((int)q.selectCols.size(), 2);
    ASSERT(q.where.active);
    ASSERT_STR(q.where.column, "ID");
    ASSERT_STR(q.where.value, "5");
}

TEST(parser_inner_join) {
    Parser p;
    auto q = p.parse("SELECT * FROM A INNER JOIN B ON A.ID = B.AID;");
    ASSERT(q.error.empty());
    ASSERT(q.join.active);
    ASSERT_STR(q.join.rightTable, "B");
    ASSERT_STR(q.join.leftCol, "ID");
    ASSERT_STR(q.join.op, "=");
    ASSERT_STR(q.join.rightCol, "AID");
}

TEST(parser_create_if_not_exists) {
    Parser p;
    auto q = p.parse("CREATE TABLE IF NOT EXISTS BENCH (ID DECIMAL, NAME VARCHAR(64));");
    ASSERT(q.error.empty());
    ASSERT(q.createIfNotExists);
    ASSERT_STR(q.tableName, "BENCH");
    ASSERT_EQ((int)q.columns.size(), 2);
}

TEST(parser_multi_insert_and_order_by) {
    Parser p;
    auto q = p.parse(
        "INSERT INTO BENCH VALUES (1,'Alice',10.5),(2,'Bob',11.5);");
    ASSERT(q.error.empty());
    ASSERT_EQ((int)q.insertRows.size(), 2);
    ASSERT_STR(q.insertRows[1][1], "Bob");

    auto s = p.parse("SELECT NAME FROM BENCH WHERE ID >= 1 ORDER BY NAME DESC;");
    ASSERT(s.error.empty());
    ASSERT(s.orderBy.active);
    ASSERT(!s.orderBy.ascending);
    ASSERT_STR(s.orderBy.column, "NAME");
}

TEST(parser_delete) {
    Parser p;
    auto q = p.parse("DELETE FROM BENCH;");
    ASSERT(q.error.empty());
    ASSERT(q.type == QueryType::DELETE_ROWS);
    ASSERT_STR(q.tableName, "BENCH");
}

TEST(inner_join_exec) {
    Env e;
    e.exec("CREATE TABLE DEPT (DID INT PRIMARY KEY, DNAME TEXT)");
    e.exec("CREATE TABLE EMP2 (EID INT PRIMARY KEY, ENAME TEXT, DEPT_ID INT)");
    e.exec("INSERT INTO DEPT VALUES (1,'Engineering')");
    e.exec("INSERT INTO DEPT VALUES (2,'Marketing')");
    e.exec("INSERT INTO EMP2 VALUES (10,'Alice',1)");
    e.exec("INSERT INTO EMP2 VALUES (11,'Bob',2)");
    e.exec("INSERT INTO EMP2 VALUES (12,'Carol',1)");

    int rows = 0;
    auto cb = [](void *d, int ncols, char **vals, char **names) -> int {
        (void)ncols;
        (void)vals;
        (void)names;
        (*(int*)d)++; return 0;
    };
    auto q = e.parser.parse(
        "SELECT * FROM EMP2 INNER JOIN DEPT ON EMP2.DEPT_ID = DEPT.DID");
    std::string err;
    e.executor.execute(q, cb, &rows, err);
    ASSERT_EQ(rows, 3);
}

TEST(order_by_exec) {
    Env e;
    e.exec("CREATE TABLE ORD (ID INT PRIMARY KEY, NAME TEXT, SCORE DECIMAL)");
    e.exec("INSERT INTO ORD VALUES (1,'Bob',12.5),(2,'Alice',10.5),(3,'Carol',15.0)");

    struct Ctx { std::vector<std::string> names; };
    Ctx ctx;
    auto cb = [](void *d, int ncols, char **vals, char **names) -> int {
        (void)names;
        if (ncols > 0) {
            static_cast<Ctx *>(d)->names.push_back(vals[0]);
        }
        return 0;
    };

    auto q = e.parser.parse("SELECT NAME FROM ORD ORDER BY NAME ASC");
    std::string err;
    e.executor.execute(q, cb, &ctx, err);
    ASSERT_EQ((int)ctx.names.size(), 3);
    ASSERT_STR(ctx.names[0], "Alice");
    ASSERT_STR(ctx.names[2], "Carol");
}

TEST(delete_all_rows) {
    Env e;
    e.exec("CREATE TABLE RESET_ME (ID INT PRIMARY KEY, NAME TEXT)");
    e.exec("INSERT INTO RESET_ME VALUES (1,'A'),(2,'B')");
    e.exec("DELETE FROM RESET_ME");

    int rows = 0;
    auto cb = [](void *d, int, char **, char **) -> int { (*(int*)d)++; return 0; };
    auto q = e.parser.parse("SELECT * FROM RESET_ME");
    std::string err;
    e.executor.execute(q, cb, &rows, err);
    ASSERT_EQ(rows, 0);
}

TEST(delete_then_reinsert_rows) {
    Env e;
    e.exec("CREATE TABLE RESET_AND_REUSE (ID INT PRIMARY KEY, NAME TEXT)");
    e.exec("INSERT INTO RESET_AND_REUSE VALUES (1,'A'),(2,'B')");
    e.exec("DELETE FROM RESET_AND_REUSE");
    e.exec("INSERT INTO RESET_AND_REUSE VALUES (3,'C')");

    const auto rows = collect_rows(e.executor, e.parser, "SELECT * FROM RESET_AND_REUSE");
    ASSERT_EQ((int)rows.size(), 1);
    ASSERT_STR(rows[0], "3|C");
}

TEST(duplicate_primary_key_rejected) {
    Env e;
    e.exec("CREATE TABLE DUPES (ID INT PRIMARY KEY, NAME TEXT NOT NULL)");
    e.exec("INSERT INTO DUPES VALUES (1,'Alice')");

    bool failed = false;
    try {
        e.exec("INSERT INTO DUPES VALUES (1,'Bob')");
    } catch (...) {
        failed = true;
    }
    ASSERT(failed);
}

TEST(invalid_projection_fails) {
    Env e;
    e.exec("CREATE TABLE PROJECT_ME (ID INT PRIMARY KEY, NAME TEXT)");
    bool failed = false;
    try {
        e.exec("SELECT UNKNOWN_COLUMN FROM PROJECT_ME");
    } catch (...) {
        failed = true;
    }
    ASSERT(failed);
}

TEST(expiration) {
    Env e;
    e.exec("CREATE TABLE TTL (ID INT PRIMARY KEY, VAL TEXT)");

    // Insert with expiry in the past (1 ms ago in Unix epoch ms)
    auto past = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 1000;

    std::string sql = "INSERT INTO TTL VALUES (1,'expired') EXPIRES " + std::to_string(past);
    e.exec(sql);
    e.exec("INSERT INTO TTL VALUES (2,'fresh')");

    int rows = 0;
    auto cb = [](void *d, int, char **, char **) -> int { (*(int*)d)++; return 0; };
    // Invalidate cache first
    e.cache.invalidateTable("TTL");
    auto q = e.parser.parse("SELECT * FROM TTL");
    std::string err;
    e.executor.execute(q, cb, &rows, err);
    ASSERT_EQ(rows, 1);  // only 'fresh' row
}

TEST(expiration_cache_respects_ttl) {
    Env e;
    e.exec("CREATE TABLE TTL_CACHE (ID INT PRIMARY KEY, VAL TEXT)");

    const auto future = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() + 100;
    e.exec("INSERT INTO TTL_CACHE VALUES (1,'soon-gone') EXPIRES " + std::to_string(future));

    int rows = 0;
    auto cb = [](void *d, int, char **, char **) -> int { (*(int*)d)++; return 0; };
    auto q = e.parser.parse("SELECT * FROM TTL_CACHE");
    std::string err;

    e.executor.execute(q, cb, &rows, err);
    ASSERT_EQ(rows, 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(250));

    rows = 0;
    e.executor.execute(q, cb, &rows, err);
    ASSERT_EQ(rows, 0);
}

TEST(expired_primary_key_can_be_reused) {
    Env e;
    e.exec("CREATE TABLE TTL_REUSE (ID INT PRIMARY KEY, VAL TEXT)");

    const auto past = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 1000;
    e.exec("INSERT INTO TTL_REUSE VALUES (1,'old') EXPIRES " + std::to_string(past));
    e.exec("INSERT INTO TTL_REUSE VALUES (1,'fresh')");

    struct Ctx { std::vector<std::string> vals; };
    Ctx ctx;
    auto cb = [](void *d, int ncols, char **vals, char **) -> int {
        if (ncols > 1) {
            static_cast<Ctx *>(d)->vals.push_back(vals[1]);
        }
        return 0;
    };
    auto q = e.parser.parse("SELECT * FROM TTL_REUSE WHERE ID = 1");
    std::string err;
    e.executor.execute(q, cb, &ctx, err);
    ASSERT_EQ((int)ctx.vals.size(), 1);
    ASSERT_STR(ctx.vals[0], "fresh");
}

TEST(snapshot_persistence_round_trip) {
    const std::string path = "/tmp/flexql_snapshot_roundtrip.bin";
    std::remove(path.c_str());

    Env writer;
    writer.exec("CREATE TABLE SNAP (ID INT PRIMARY KEY, NAME TEXT)");
    writer.exec("INSERT INTO SNAP VALUES (1,'Alice')");

    const auto past = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 1000;
    writer.exec("INSERT INTO SNAP VALUES (2,'Expired') EXPIRES " + std::to_string(past));

    std::string err;
    ASSERT(persistence::saveSnapshot(path, writer.storage, err));

    Env reader;
    ASSERT(persistence::loadSnapshot(path, reader.storage, reader.executor, err));

    struct Ctx { std::vector<std::string> names; };
    Ctx ctx;
    auto cb = [](void *d, int ncols, char **vals, char **names) -> int {
        for (int i = 0; i < ncols; ++i) {
            if (std::string(names[i]) == "NAME") {
                static_cast<Ctx *>(d)->names.push_back(vals[i]);
            }
        }
        return 0;
    };

    auto q = reader.parser.parse("SELECT * FROM SNAP ORDER BY ID ASC");
    reader.executor.execute(q, cb, &ctx, err);
    ASSERT_EQ((int)ctx.names.size(), 1);
    ASSERT_STR(ctx.names[0], "Alice");

    std::remove(path.c_str());
}

TEST(durable_log_round_trip) {
    const std::string path = "/tmp/flexql_durable_roundtrip.wal";
    std::remove(path.c_str());

    StorageEngine writerStorage;
    LRUCache writerCache(64);
    std::string err;
    DurableLog writerLog(path);
    ASSERT(writerLog.open(err));

    Executor writer(writerStorage, writerCache, &writerLog);
    Parser parser;
    exec_sql(writer, parser, "CREATE TABLE WAL_USERS (ID INT PRIMARY KEY, NAME TEXT)");
    exec_sql(writer, parser, "INSERT INTO WAL_USERS VALUES (1,'Alice')");
    exec_sql(writer, parser, "DELETE FROM WAL_USERS");
    exec_sql(writer, parser, "INSERT INTO WAL_USERS VALUES (2,'Bob')");

    const auto past = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count() - 1000;
    exec_sql(writer, parser,
             "INSERT INTO WAL_USERS VALUES (3,'Expired') EXPIRES " + std::to_string(past));
    writerLog.close();

    StorageEngine readerStorage;
    ASSERT(replayDurableLog(path, readerStorage, err));

    LRUCache readerCache(64);
    Executor reader(readerStorage, readerCache);
    rebuild_all_indexes(readerStorage, reader);

    const auto rows = collect_rows(reader, parser,
                                   "SELECT * FROM WAL_USERS ORDER BY ID ASC");
    ASSERT_EQ((int)rows.size(), 1);
    ASSERT_STR(rows[0], "2|Bob");

    std::remove(path.c_str());
}

TEST(durable_log_replay_ignores_truncated_tail) {
    const std::string path = "/tmp/flexql_durable_truncated.wal";
    std::remove(path.c_str());

    StorageEngine writerStorage;
    LRUCache writerCache(64);
    std::string err;
    DurableLog writerLog(path);
    ASSERT(writerLog.open(err));

    Executor writer(writerStorage, writerCache, &writerLog);
    Parser parser;
    exec_sql(writer, parser, "CREATE TABLE WAL_CRASH (ID INT PRIMARY KEY, NAME TEXT)");
    exec_sql(writer, parser, "INSERT INTO WAL_CRASH VALUES (1,'Alice')");
    writerLog.close();

    std::FILE *file = std::fopen(path.c_str(), "ab");
    ASSERT(file != nullptr);
    const unsigned char tail[] = {0xAA, 0xBB, 0xCC};
    const size_t written = std::fwrite(tail, 1, sizeof(tail), file);
    ASSERT_EQ(written, sizeof(tail));
    const int closeRc = std::fclose(file);
    ASSERT_EQ(closeRc, 0);

    StorageEngine readerStorage;
    ASSERT(replayDurableLog(path, readerStorage, err));

    LRUCache readerCache(64);
    Executor reader(readerStorage, readerCache);
    rebuild_all_indexes(readerStorage, reader);

    const auto rows = collect_rows(reader, parser,
                                   "SELECT * FROM WAL_CRASH WHERE ID = 1");
    ASSERT_EQ((int)rows.size(), 1);
    ASSERT_STR(rows[0], "1|Alice");

    std::remove(path.c_str());
}

TEST(durable_log_replay_requires_commit_marker) {
    const std::string path = "/tmp/flexql_durable_commit_marker.wal";
    std::remove(path.c_str());

    std::string err;
    DurableLog log(path);
    ASSERT(log.open(err));

    Parser parser;
    uint64_t txId = 0;

    auto pendingCreate = parser.parse("CREATE TABLE WAL_PENDING (ID INT PRIMARY KEY, NAME TEXT)");
    ASSERT(pendingCreate.error.empty());
    ASSERT(log.beginCreate(pendingCreate, txId, err));

    auto committedCreate = parser.parse("CREATE TABLE WAL_USERS (ID INT PRIMARY KEY, NAME TEXT)");
    ASSERT(committedCreate.error.empty());
    txId = 0;
    ASSERT(log.beginCreate(committedCreate, txId, err));
    ASSERT(log.commit(txId, err));

    auto pendingInsert = parser.parse("INSERT INTO WAL_USERS VALUES (1,'Alice')");
    ASSERT(pendingInsert.error.empty());
    txId = 0;
    ASSERT(log.beginInsert(pendingInsert, txId, err));

    auto committedInsert = parser.parse("INSERT INTO WAL_USERS VALUES (2,'Bob')");
    ASSERT(committedInsert.error.empty());
    txId = 0;
    ASSERT(log.beginInsert(committedInsert, txId, err));
    ASSERT(log.commit(txId, err));
    log.close();

    StorageEngine readerStorage;
    ASSERT(replayDurableLog(path, readerStorage, err));
    ASSERT(!readerStorage.tableExists("WAL_PENDING"));

    LRUCache readerCache(64);
    Executor reader(readerStorage, readerCache);
    rebuild_all_indexes(readerStorage, reader);

    const auto rows = collect_rows(reader, parser, "SELECT * FROM WAL_USERS ORDER BY ID ASC");
    ASSERT_EQ((int)rows.size(), 1);
    ASSERT_STR(rows[0], "2|Bob");

    std::remove(path.c_str());
}

TEST(server_session_honors_abort_frames) {
    Env e;
    e.exec("CREATE TABLE STREAM_ABORT (ID INT PRIMARY KEY NOT NULL, NAME TEXT NOT NULL)");

    std::string insertSql = "INSERT INTO STREAM_ABORT VALUES ";
    const int totalRows = 2000;
    for (int i = 0; i < totalRows; ++i) {
        if (i > 0) {
            insertSql += ",";
        }
        insertSql += "(" + std::to_string(i) + ",'user" + std::to_string(i) + "')";
    }
    e.exec(insertSql);

    int fds[2] = {-1, -1};
    open_socket_pair(fds);
    std::thread serverThread([&]() {
        set_socket_timeout(fds[1], 2000);
        serveClientConnection(fds[1], e.executor);
    });

    set_socket_timeout(fds[0], 2000);
    ASSERT(net::sendMsg(fds[0], std::string(protocol::kQueryPrefix) +
                                  "SELECT ID, NAME FROM STREAM_ABORT"));

    std::string frame;
    ASSERT(net::recvMsg(fds[0], frame));
    ASSERT(frame.rfind(protocol::kHeaderPrefix, 0) == 0);

    int rowFrames = 0;
    bool abortSent = false;
    while (true) {
        ASSERT(net::recvMsg(fds[0], frame));
        if (frame.rfind(protocol::kRowPrefix, 0) == 0) {
            ++rowFrames;
            if (!abortSent) {
                ASSERT(net::sendMsg(fds[0], protocol::kAbort));
                abortSent = true;
            }
            continue;
        }
        ASSERT_STR(frame.c_str(), protocol::kAborted);
        break;
    }

    ASSERT(abortSent);
    ASSERT(rowFrames >= 1);
    ASSERT(rowFrames < totalRows);
    ASSERT(net::sendMsg(fds[0], protocol::kExit));
    ::close(fds[0]);
    serverThread.join();
}

TEST(client_exec_streams_rows_and_sends_abort) {
    int fds[2] = {-1, -1};
    open_socket_pair(fds);
    std::atomic<bool> sawAbort{false};
    std::atomic<bool> sawQuery{false};
    std::thread serverThread([&]() {
        set_socket_timeout(fds[1], 2000);
        std::string frame;
        if (!net::recvMsg(fds[1], frame)) {
            ::close(fds[1]);
            return;
        }
        sawQuery = (frame == std::string(protocol::kQueryPrefix) + "SELECT ID, NAME FROM STREAM");
        net::sendMsg(fds[1], std::string(protocol::kHeaderPrefix) + "ID\tNAME");
        net::sendMsg(fds[1], std::string(protocol::kRowPrefix) + "1\tAlice");
        if (net::recvMsg(fds[1], frame) && frame == protocol::kAbort) {
            sawAbort = true;
            net::sendMsg(fds[1], protocol::kAborted);
        }
        net::recvMsg(fds[1], frame);
        ::close(fds[1]);
    });

    set_socket_timeout(fds[0], 2000);
    FlexQL *db = nullptr;
    ASSERT(flexql_open_fd_for_test(fds[0], &db) == FLEXQL_OK);

    struct Ctx {
        int rows = 0;
        std::string firstRow;
        bool headerOk = false;
    } ctx;

    auto cb = [](void *data, int ncols, char **vals, char **names) -> int {
        auto *ctx = static_cast<Ctx *>(data);
        ctx->headerOk =
            (ncols == 2 &&
             std::string(names[0]) == "ID" &&
             std::string(names[1]) == "NAME");
        ++ctx->rows;
        ctx->firstRow = std::string(vals[0]) + "|" + vals[1];
        return 1;
    };

    char *errMsg = nullptr;
    ASSERT(flexql_exec(db, "SELECT ID, NAME FROM STREAM", cb, &ctx, &errMsg) == FLEXQL_OK);
    ASSERT(errMsg == nullptr);
    ASSERT_EQ(ctx.rows, 1);
    ASSERT_STR(ctx.firstRow.c_str(), "1|Alice");
    ASSERT(ctx.headerOk);
    ASSERT(sawQuery.load());
    ASSERT(sawAbort.load());

    ASSERT(flexql_close(db) == FLEXQL_OK);
    serverThread.join();
}

TEST(cache_invalidation_on_insert) {
    Env e;
    e.exec("CREATE TABLE CI (ID INT PRIMARY KEY, V TEXT)");
    e.exec("INSERT INTO CI VALUES (1,'A')");

    int rows = 0;
    auto cb = [](void *d, int, char **, char **) -> int { (*(int*)d)++; return 0; };

    // First SELECT -> populates cache
    auto q = e.parser.parse("SELECT * FROM CI");
    std::string err;
    e.executor.execute(q, cb, &rows, err);
    ASSERT_EQ(rows, 1);

    // Insert more
    e.exec("INSERT INTO CI VALUES (2,'B')");

    // SELECT again -> cache should be invalidated, should return 2
    rows = 0;
    e.executor.execute(q, cb, &rows, err);
    ASSERT_EQ(rows, 2);
}

TEST(concurrent_inserts) {
    Env e;
    e.exec("CREATE TABLE CONC (ID INT PRIMARY KEY, VAL TEXT)");

    const int N = 1000;
    std::vector<std::thread> threads;
    std::atomic<int> counter{0};

    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&, i]() {
            std::string sql = "INSERT INTO CONC VALUES (" +
                              std::to_string(i) + ",'val" +
                              std::to_string(i) + "')";
            try {
                Env localEnv;  // Each thread uses its own parser
                Parser p;
                auto q = p.parse(sql);
                std::string err;
                // Use shared storage/executor
                e.executor.execute(q, nullptr, nullptr, err);
                counter++;
            } catch (...) {}
        });
    }
    for (auto &t : threads) t.join();
    ASSERT(counter.load() == N);
}

TEST(select_specific_cols) {
    Env e;
    e.exec("CREATE TABLE SC (ID INT PRIMARY KEY, NAME TEXT, AGE INT)");
    e.exec("INSERT INTO SC VALUES (1,'Alice',30)");
    e.exec("INSERT INTO SC VALUES (2,'Bob',25)");

    struct Ctx { std::vector<std::vector<std::string>> rows; int ncols = 0; };
    Ctx ctx;
    auto cb = [](void *d, int ncols, char **vals, char **names) -> int {
        (void)names;
        auto *c = (Ctx*)d;
        c->ncols = ncols;
        std::vector<std::string> row;
        for (int i = 0; i < ncols; ++i) row.push_back(vals[i]);
        c->rows.push_back(row);
        return 0;
    };
    auto q = e.parser.parse("SELECT NAME, AGE FROM SC");
    std::string err;
    e.executor.execute(q, cb, &ctx, err);
    ASSERT_EQ((int)ctx.rows.size(), 2);
    ASSERT_EQ(ctx.ncols, 2);
    ASSERT_STR(ctx.rows[0][0], "Alice");
}

TEST(index_spill_falls_back_to_disk_scan) {
    const char *envName = "FLEXQL_MAX_INDEXED_ROWS";
    const char *previous = std::getenv(envName);
    const std::string previousValue = previous != nullptr ? previous : "";
    const bool hadPrevious = previous != nullptr;
    setenv(envName, "8", 1);

    struct EnvGuard {
        const char *name;
        std::string oldValue;
        bool hadOldValue = false;
        ~EnvGuard() {
            if (hadOldValue) {
                setenv(name, oldValue.c_str(), 1);
            } else {
                unsetenv(name);
            }
        }
    } guard{envName, previousValue, hadPrevious};

    StorageEngine storage;
    LRUCache cache(64);
    Executor executor(storage, cache);
    Parser parser;

    exec_sql(executor, parser, "CREATE TABLE BIG_SCAN (ID INT PRIMARY KEY NOT NULL, VAL TEXT NOT NULL)");
    for (int i = 0; i < 32; ++i) {
        exec_sql(executor, parser,
                 "INSERT INTO BIG_SCAN VALUES (" + std::to_string(i) +
                 ",'v" + std::to_string(i) + "')");
    }

    Table *table = storage.getTable("BIG_SCAN");
    ASSERT(table != nullptr);
    ASSERT(storage.rowCount(*table) == 32u);

    const auto rows = collect_rows(executor, parser, "SELECT * FROM BIG_SCAN WHERE ID = 31");
    ASSERT_EQ((int)rows.size(), 1);
    ASSERT_STR(rows[0], "31|v31");
}

TEST(primary_key_disk_index_handles_spill) {
    const char *envName = "FLEXQL_MAX_INDEXED_ROWS";
    const char *previous = std::getenv(envName);
    const std::string previousValue = previous != nullptr ? previous : "";
    const bool hadPrevious = previous != nullptr;
    setenv(envName, "8", 1);

    struct EnvGuard {
        const char *name;
        std::string oldValue;
        bool hadOldValue = false;
        ~EnvGuard() {
            if (hadOldValue) {
                setenv(name, oldValue.c_str(), 1);
            } else {
                unsetenv(name);
            }
        }
    } guard{envName, previousValue, hadPrevious};

    StorageEngine storage;
    LRUCache cache(64);
    Executor executor(storage, cache);
    Parser parser;

    exec_sql(executor, parser,
             "CREATE TABLE BIG_PK (ID INT PRIMARY KEY NOT NULL, VAL TEXT NOT NULL)");
    for (int i = 0; i < 256; ++i) {
        exec_sql(executor, parser,
                 "INSERT INTO BIG_PK VALUES (" + std::to_string(i) +
                 ",'v" + std::to_string(i) + "')");
    }

    Table *table = storage.getTable("BIG_PK");
    ASSERT(table != nullptr);
    ASSERT(storage.hasPrimaryKeyIndex(*table));

    uint64_t rowOffset = UINT64_MAX;
    Row row;
    ASSERT(storage.lookupPrimaryKey(*table, "255", rowOffset, &row));
    ASSERT(rowOffset != UINT64_MAX);
    ASSERT_STR(row.values[1], "v255");

    const auto duplicate = parser.parse("INSERT INTO BIG_PK VALUES (255,'dupe')");
    std::string err;
    ASSERT(executor.execute(duplicate, nullptr, nullptr, err) == FLEXQL_ERROR);
}

/* ─── Performance smoke test ─────────────────────────────────────────────── */

TEST(perf_100k_inserts) {
    Env e;
    e.exec("CREATE TABLE PERF (ID INT PRIMARY KEY NOT NULL, VAL TEXT NOT NULL)");

    const int N = 100000;
    auto t0 = std::chrono::high_resolution_clock::now();

    Parser p;
    for (int i = 0; i < N; ++i) {
        std::string sql = "INSERT INTO PERF VALUES (" +
                          std::to_string(i) + ",'value" +
                          std::to_string(i) + "')";
        auto q = p.parse(sql);
        std::string err;
        e.executor.execute(q, nullptr, nullptr, err);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "      " << N << " inserts in " << ms << " ms ("
              << (int)(N / (ms/1000)) << " rows/sec)\n";
    ASSERT(ms < 30000); // must complete in < 30s
}

TEST(perf_pk_lookup) {
    Env e;
    e.exec("CREATE TABLE LOOKUP (ID INT PRIMARY KEY NOT NULL, NAME TEXT)");
    const int N = 50000;
    Parser p;
    for (int i = 0; i < N; ++i) {
        auto q = p.parse("INSERT INTO LOOKUP VALUES (" +
                         std::to_string(i) + ",'n" + std::to_string(i) + "')");
        std::string err;
        e.executor.execute(q, nullptr, nullptr, err);
    }
    e.cache.clear();

    auto t0 = std::chrono::high_resolution_clock::now();
    int found = 0;
    auto cb = [](void *d, int, char **, char **) -> int { (*(int*)d)++; return 0; };
    for (int i = 0; i < 1000; ++i) {
        auto q = p.parse("SELECT * FROM LOOKUP WHERE ID = " + std::to_string(i * 49));
        std::string err;
        e.executor.execute(q, cb, &found, err);
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "      1000 PK lookups in " << ms << " ms\n";
    ASSERT_EQ(found, 1000);
}

/* ─── main ────────────────────────────────────────────────────────────────── */

int main() {
    std::cout << "=== FlexQL Test Suite ===\n\n";

    std::cout << "-- Parser --\n";
    RUN(parser_create);
    RUN(parser_insert);
    RUN(parser_select_where);
    RUN(parser_inner_join);
    RUN(parser_create_if_not_exists);
    RUN(parser_multi_insert_and_order_by);
    RUN(parser_delete);

    std::cout << "\n-- Storage --\n";
    RUN(storage_create);
    RUN(storage_insert_select);

    std::cout << "\n-- Index --\n";
    RUN(hash_index);

    std::cout << "\n-- Cache --\n";
    RUN(lru_cache);
    RUN(cache_invalidation_on_insert);

    std::cout << "\n-- Executor --\n";
    RUN(where_equality);
    RUN(inner_join_exec);
    RUN(order_by_exec);
    RUN(expiration);
    RUN(expiration_cache_respects_ttl);
    RUN(expired_primary_key_can_be_reused);
    RUN(select_specific_cols);
    RUN(index_spill_falls_back_to_disk_scan);
    RUN(delete_all_rows);
    RUN(delete_then_reinsert_rows);
    RUN(duplicate_primary_key_rejected);
    RUN(invalid_projection_fails);
    RUN(primary_key_disk_index_handles_spill);

    std::cout << "\n-- Durability --\n";
    RUN(snapshot_persistence_round_trip);
    RUN(durable_log_round_trip);
    RUN(durable_log_replay_ignores_truncated_tail);
    RUN(durable_log_replay_requires_commit_marker);

    std::cout << "\n-- Concurrency --\n";
    RUN(concurrent_inserts);

    std::cout << "\n-- Network --\n";
    RUN(server_session_honors_abort_frames);
    RUN(client_exec_streams_rows_and_sends_abort);

    std::cout << "\n-- Performance --\n";
    RUN(perf_100k_inserts);
    RUN(perf_pk_lookup);

    std::cout << "\n=== Results: " << g_pass << " passed, "
              << g_fail << " failed ===\n";
    return g_fail ? 1 : 0;
}
