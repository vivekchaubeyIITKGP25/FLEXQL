#include "durable_log.h"
#include "executor.h"
#include "flexql.h"
#include "hash_index.h"
#include "lru_cache.h"
#include "parser.h"
#include "persistence.h"
#include "storage.h"

#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;

#define TEST(name) void test_##name()
#define RUN(name) do { \
    std::cout << "  [ RUN ] " #name "\n"; \
    try { \
        test_##name(); \
        std::cout << "  [  OK ] " #name "\n"; \
        ++g_pass; \
    } catch (const std::exception &e) { \
        std::cout << "  [FAIL ] " #name ": " << e.what() << "\n"; \
        ++g_fail; \
    } catch (...) { \
        std::cout << "  [FAIL ] " #name ": unknown exception\n"; \
        ++g_fail; \
    } \
} while (0)

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        throw std::runtime_error("ASSERT failed: " #cond); \
    } \
} while (0)

#define ASSERT_EQ(a, b) do { \
    auto lhs = (a); \
    auto rhs = (b); \
    if (lhs != rhs) { \
        throw std::runtime_error(std::string("ASSERT_EQ failed: ") + \
                                 std::to_string(lhs) + " != " + std::to_string(rhs)); \
    } \
} while (0)

struct Env {
    StorageEngine storage;
    LRUCache cache{256};
    Executor executor{storage, cache};
    Parser parser;

    void exec(const std::string &sql) {
        auto q = parser.parse(sql);
        if (!q.error.empty()) {
            throw std::runtime_error("Parse error: " + q.error + " | SQL: " + sql);
        }

        std::string err;
        const int rc = executor.execute(q, nullptr, nullptr, err);
        if (rc != FLEXQL_OK) {
            throw std::runtime_error("Exec error: " + err + " | SQL: " + sql);
        }
    }
};

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
        throw std::runtime_error("Parse error: " + q.error + " | SQL: " + sql);
    }

    std::string err;
    const int rc = executor.execute(q, cb, &ctx, err);
    if (rc != FLEXQL_OK) {
        throw std::runtime_error("Exec error: " + err + " | SQL: " + sql);
    }

    return ctx.rows;
}

static void assert_rows_equal(const std::vector<std::string> &actual,
                              std::initializer_list<const char *> expected,
                              const std::string &label) {
    std::vector<std::string> expected_rows;
    expected_rows.reserve(expected.size());
    for (const char *row : expected) {
        expected_rows.emplace_back(row);
    }

    if (actual == expected_rows) {
        return;
    }

    std::string message = label + " mismatch. Expected [";
    for (size_t i = 0; i < expected_rows.size(); ++i) {
        if (i > 0) {
            message += ", ";
        }
        message += expected_rows[i];
    }
    message += "] got [";
    for (size_t i = 0; i < actual.size(); ++i) {
        if (i > 0) {
            message += ", ";
        }
        message += actual[i];
    }
    message += "]";
    throw std::runtime_error(message);
}

static bool exec_fails(Env &env, const std::string &sql) {
    auto q = env.parser.parse(sql);
    if (!q.error.empty()) {
        return true;
    }

    std::string err;
    return env.executor.execute(q, nullptr, nullptr, err) != FLEXQL_OK;
}

static void rebuild_all_indexes(StorageEngine &storage, Executor &executor) {
    for (const std::string &table_name : storage.listTables()) {
        executor.rebuildIndex(table_name);
    }
}

TEST(sql_crud_and_comparisons) {
    Env e;
    e.exec("CREATE TABLE IF NOT EXISTS LEDGER (ID INT PRIMARY KEY NOT NULL, NAME TEXT NOT NULL, SCORE DECIMAL NOT NULL)");
    e.exec("CREATE TABLE IF NOT EXISTS LEDGER (ID INT PRIMARY KEY NOT NULL, NAME TEXT NOT NULL, SCORE DECIMAL NOT NULL)");
    e.exec("INSERT INTO LEDGER VALUES (1,'Alice',10),(2,'Bob',20),(3,'Cara',30),(4,'Drew',40)");

    assert_rows_equal(
        collect_rows(e.executor, e.parser, "SELECT NAME, SCORE FROM LEDGER ORDER BY SCORE DESC"),
        {"Drew|40", "Cara|30", "Bob|20", "Alice|10"},
        "ORDER BY DESC");

    assert_rows_equal(
        collect_rows(e.executor, e.parser, "SELECT ID FROM LEDGER WHERE SCORE = 20"),
        {"2"},
        "WHERE =");

    assert_rows_equal(
        collect_rows(e.executor, e.parser, "SELECT ID FROM LEDGER WHERE SCORE != 20 ORDER BY ID ASC"),
        {"1", "3", "4"},
        "WHERE !=");

    assert_rows_equal(
        collect_rows(e.executor, e.parser, "SELECT ID FROM LEDGER WHERE SCORE < 30 ORDER BY ID ASC"),
        {"1", "2"},
        "WHERE <");

    assert_rows_equal(
        collect_rows(e.executor, e.parser, "SELECT ID FROM LEDGER WHERE SCORE <= 30 ORDER BY ID ASC"),
        {"1", "2", "3"},
        "WHERE <=");

    assert_rows_equal(
        collect_rows(e.executor, e.parser, "SELECT ID FROM LEDGER WHERE SCORE > 20 ORDER BY ID ASC"),
        {"3", "4"},
        "WHERE >");

    assert_rows_equal(
        collect_rows(e.executor, e.parser, "SELECT ID FROM LEDGER WHERE SCORE >= 30 ORDER BY ID ASC"),
        {"3", "4"},
        "WHERE >=");

    e.exec("DELETE FROM LEDGER");
    ASSERT_EQ(collect_rows(e.executor, e.parser, "SELECT * FROM LEDGER").size(), 0u);
}

TEST(join_projection_and_ordering) {
    Env e;
    e.exec("CREATE TABLE USERS (ID INT PRIMARY KEY NOT NULL, NAME TEXT NOT NULL)");
    e.exec("CREATE TABLE ORDERS (ORDER_ID INT PRIMARY KEY NOT NULL, USER_ID INT NOT NULL, AMOUNT DECIMAL NOT NULL)");
    e.exec("INSERT INTO USERS VALUES (1,'Alice'),(2,'Bob'),(3,'Cara')");
    e.exec("INSERT INTO ORDERS VALUES (101,1,150),(102,2,75),(103,3,300)");

    assert_rows_equal(
        collect_rows(
            e.executor,
            e.parser,
            "SELECT USERS.NAME, ORDERS.AMOUNT FROM USERS "
            "INNER JOIN ORDERS ON USERS.ID = ORDERS.USER_ID "
            "WHERE ORDERS.AMOUNT >= 100 ORDER BY ORDERS.AMOUNT DESC"),
        {"Cara|300", "Alice|150"},
        "JOIN with projection, filter, and ORDER BY");
}

TEST(ttl_cache_and_primary_key_reuse) {
    Env e;
    e.exec("CREATE TABLE TTL_CASE (ID INT PRIMARY KEY NOT NULL, VALUE TEXT NOT NULL)");

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    e.exec("INSERT INTO TTL_CASE VALUES (1,'expired') EXPIRES " + std::to_string(now_ms - 1000));
    e.exec("INSERT INTO TTL_CASE VALUES (1,'fresh')");

    assert_rows_equal(
        collect_rows(e.executor, e.parser, "SELECT * FROM TTL_CASE WHERE ID = 1"),
        {"1|fresh"},
        "Expired primary key reuse");

    e.exec("CREATE TABLE TTL_CACHE (ID INT PRIMARY KEY NOT NULL, VALUE TEXT NOT NULL)");
    e.exec("INSERT INTO TTL_CACHE VALUES (1,'soon-gone') EXPIRES " + std::to_string(now_ms + 100));

    assert_rows_equal(
        collect_rows(e.executor, e.parser, "SELECT * FROM TTL_CACHE"),
        {"1|soon-gone"},
        "TTL visible before expiry");

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    ASSERT_EQ(collect_rows(e.executor, e.parser, "SELECT * FROM TTL_CACHE").size(), 0u);
}

TEST(failure_paths) {
    Env e;
    e.exec("CREATE TABLE FAIL_CASE (ID INT PRIMARY KEY NOT NULL, NAME TEXT NOT NULL)");
    e.exec("INSERT INTO FAIL_CASE VALUES (1,'Alice')");

    ASSERT_TRUE(exec_fails(e, "CREATE TABLE FAIL_CASE (ID INT PRIMARY KEY NOT NULL, NAME TEXT NOT NULL)"));
    ASSERT_TRUE(exec_fails(e, "INSERT INTO FAIL_CASE VALUES (1,'Bob')"));
    ASSERT_TRUE(exec_fails(e, "SELECT UNKNOWN FROM FAIL_CASE"));
    ASSERT_TRUE(exec_fails(e, "SELECT * FROM MISSING_TABLE"));
    ASSERT_TRUE(exec_fails(e, "SELECT * FROM FAIL_CASE WHERE ID = 1 AND NAME = 'Alice'"));
}

TEST(durability_round_trip) {
    const std::string snapshot_path = "/tmp/flexql_all_cases_snapshot.bin";
    const std::string wal_path = "/tmp/flexql_all_cases.wal";
    std::remove(snapshot_path.c_str());
    std::remove(wal_path.c_str());

    {
        Env writer;
        writer.exec("CREATE TABLE SNAP (ID INT PRIMARY KEY NOT NULL, NAME TEXT NOT NULL)");
        writer.exec("INSERT INTO SNAP VALUES (1,'Alice')");

        const auto past_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() - 1000;
        writer.exec("INSERT INTO SNAP VALUES (2,'Expired') EXPIRES " + std::to_string(past_ms));

        std::string err;
        ASSERT_TRUE(persistence::saveSnapshot(snapshot_path, writer.storage, err));

        Env reader;
        ASSERT_TRUE(persistence::loadSnapshot(snapshot_path, reader.storage, reader.executor, err));
        assert_rows_equal(
            collect_rows(reader.executor, reader.parser, "SELECT * FROM SNAP ORDER BY ID ASC"),
            {"1|Alice"},
            "Snapshot round trip");
    }

    {
        StorageEngine writer_storage;
        LRUCache writer_cache(64);
        std::string err;
        DurableLog writer_log(wal_path);
        ASSERT_TRUE(writer_log.open(err));

        Executor writer_executor(writer_storage, writer_cache, &writer_log);
        Parser parser;

        auto exec_sql = [&](const std::string &sql) {
            auto q = parser.parse(sql);
            if (!q.error.empty()) {
                throw std::runtime_error("Parse error: " + q.error + " | SQL: " + sql);
            }

            std::string local_err;
            const int rc = writer_executor.execute(q, nullptr, nullptr, local_err);
            if (rc != FLEXQL_OK) {
                throw std::runtime_error("Exec error: " + local_err + " | SQL: " + sql);
            }
        };

        exec_sql("CREATE TABLE WAL_USERS (ID INT PRIMARY KEY NOT NULL, NAME TEXT NOT NULL)");
        exec_sql("INSERT INTO WAL_USERS VALUES (1,'Alice')");
        exec_sql("DELETE FROM WAL_USERS");
        exec_sql("INSERT INTO WAL_USERS VALUES (2,'Bob')");
        writer_log.close();

        StorageEngine reader_storage;
        ASSERT_TRUE(replayDurableLog(wal_path, reader_storage, err));
        LRUCache reader_cache(64);
        Executor reader_executor(reader_storage, reader_cache);
        rebuild_all_indexes(reader_storage, reader_executor);

        assert_rows_equal(
            collect_rows(reader_executor, parser, "SELECT * FROM WAL_USERS ORDER BY ID ASC"),
            {"2|Bob"},
            "Durable log round trip");
    }

    std::remove(snapshot_path.c_str());
    std::remove(wal_path.c_str());
}

int main() {
    std::cout << "=== FlexQL All Cases Test Suite ===\n\n";

    RUN(sql_crud_and_comparisons);
    RUN(join_projection_and_ordering);
    RUN(ttl_cache_and_primary_key_reuse);
    RUN(failure_paths);
    RUN(durability_round_trip);

    std::cout << "\n=== Results: " << g_pass << " passed, " << g_fail << " failed ===\n";
    return g_fail ? 1 : 0;
}
