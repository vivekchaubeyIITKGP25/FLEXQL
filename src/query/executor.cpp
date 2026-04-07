/*
 * FlexQL: Query executor implementation.
 */

#include "executor.h"

#include "durable_log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::string trimCopy(const std::string &s) {
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string upperCopy(const std::string &s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return out;
}

std::pair<std::string, std::string> splitQualifiedName(const std::string &ref) {
    const std::string trimmed = trimCopy(ref);
    const size_t dotPos = trimmed.find('.');
    if (dotPos == std::string::npos) {
        return {"", trimmed};
    }
    return {
        trimCopy(trimmed.substr(0, dotPos)),
        trimCopy(trimmed.substr(dotPos + 1))
    };
}

bool sameIdentifier(const std::string &lhs, const std::string &rhs) {
    return upperCopy(lhs) == upperCopy(rhs);
}

bool isNullLiteral(const std::string &rawValue) {
    const std::string trimmed = trimCopy(rawValue);
    if (trimmed.size() >= 2 &&
        ((trimmed.front() == '\'' && trimmed.back() == '\'') ||
         (trimmed.front() == '"' && trimmed.back() == '"'))) {
        return false;
    }
    return upperCopy(trimmed) == "NULL";
}

bool parseIntegerStrict(const std::string &text) {
    char *end = nullptr;
    const char *start = text.c_str();
    std::strtoll(start, &end, 10);
    return start != end && end != nullptr && *end == '\0';
}

bool parseDoubleStrict(const std::string &text) {
    char *end = nullptr;
    const char *start = text.c_str();
    std::strtod(start, &end);
    return start != end && end != nullptr && *end == '\0';
}

size_t parseSizeEnv(const char *name, size_t fallback) {
    const char *raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') {
        return fallback;
    }

    char *end = nullptr;
    const unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (end == raw || end == nullptr || *end != '\0') {
        return fallback;
    }
    return static_cast<size_t>(parsed);
}

int compareValues(const std::string &lhs, const std::string &rhs) {
    if (parseDoubleStrict(lhs) && parseDoubleStrict(rhs)) {
        const double left = std::strtod(lhs.c_str(), nullptr);
        const double right = std::strtod(rhs.c_str(), nullptr);
        if (left < right) {
            return -1;
        }
        if (left > right) {
            return 1;
        }
        return 0;
    }
    if (lhs < rhs) {
        return -1;
    }
    if (lhs > rhs) {
        return 1;
    }
    return 0;
}

bool compareWithOperator(const std::string &lhs,
                         const std::string &op,
                         const std::string &rhs) {
    const int cmp = compareValues(lhs, rhs);
    if (op == "=") {
        return cmp == 0;
    }
    if (op == "!=") {
        return cmp != 0;
    }
    if (op == "<") {
        return cmp < 0;
    }
    if (op == ">") {
        return cmp > 0;
    }
    if (op == "<=") {
        return cmp <= 0;
    }
    if (op == ">=") {
        return cmp >= 0;
    }
    return false;
}

bool rowExpired(const Row &row, int64_t nowMs) {
    return row.expires_at != 0 && row.expires_at <= nowMs;
}

struct ResolvedColumn {
    bool        fromRight = false;
    int         index = -1;
    std::string outputName;
};

struct ResolvedFilter {
    bool        active = false;
    bool        fromRight = false;
    int         index = -1;
    std::string op;
    std::string value;
};

struct ResolvedSort {
    bool active = false;
    ResolvedColumn column;
    bool ascending = true;
};

struct ResultRow {
    std::vector<std::string> values;
    std::string              sortValue;
    int64_t                  expiresAt = 0;
};

int64_t combineExpiry(int64_t left, int64_t right) {
    if (left == 0) {
        return right;
    }
    if (right == 0) {
        return left;
    }
    return std::min(left, right);
}

bool filterExpiredCachedRows(CachedResult &result, int64_t nowMs) {
    if (result.rows.empty() || result.rowExpiresAt.empty()) {
        return false;
    }

    const size_t limit = std::min(result.rows.size(), result.rowExpiresAt.size());
    std::vector<std::vector<std::string>> keptRows;
    std::vector<int64_t> keptExpiries;
    keptRows.reserve(limit);
    keptExpiries.reserve(limit);

    bool changed = (limit != result.rows.size()) || (limit != result.rowExpiresAt.size());
    for (size_t i = 0; i < limit; ++i) {
        if (result.rowExpiresAt[i] != 0 && result.rowExpiresAt[i] <= nowMs) {
            changed = true;
            continue;
        }
        keptRows.push_back(result.rows[i]);
        keptExpiries.push_back(result.rowExpiresAt[i]);
    }

    if (changed) {
        result.rows.swap(keptRows);
        result.rowExpiresAt.swap(keptExpiries);
    }
    return changed;
}

int findColumnIndexCaseInsensitive(const Table &tbl, const std::string &name) {
    for (size_t i = 0; i < tbl.schema.size(); ++i) {
        if (sameIdentifier(tbl.schema[i].name, name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool resolvePlainColumn(const Table &tbl,
                        const std::string &tableName,
                        const std::string &ref,
                        ResolvedColumn &resolved,
                        std::string &err) {
    const auto qualified = splitQualifiedName(ref);
    if (!qualified.first.empty() && !sameIdentifier(qualified.first, tableName)) {
        err = "Unknown column: " + ref;
        return false;
    }

    const int index = findColumnIndexCaseInsensitive(tbl, qualified.second);
    if (index < 0) {
        err = "Unknown column: " + ref;
        return false;
    }

    resolved.fromRight = false;
    resolved.index = index;
    resolved.outputName = trimCopy(ref);
    if (resolved.outputName.empty() || qualified.first.empty()) {
        resolved.outputName = tbl.schema[index].name;
    }
    return true;
}

bool resolveJoinColumn(const Table &left,
                       const Table &right,
                       const std::string &leftTableName,
                       const std::string &rightTableName,
                       const std::string &ref,
                       ResolvedColumn &resolved,
                       std::string &err) {
    const auto qualified = splitQualifiedName(ref);
    if (!qualified.first.empty()) {
        if (sameIdentifier(qualified.first, leftTableName)) {
            const int index = findColumnIndexCaseInsensitive(left, qualified.second);
            if (index < 0) {
                err = "Unknown column: " + ref;
                return false;
            }
            resolved.fromRight = false;
            resolved.index = index;
            resolved.outputName = trimCopy(ref);
            return true;
        }
        if (sameIdentifier(qualified.first, rightTableName)) {
            const int index = findColumnIndexCaseInsensitive(right, qualified.second);
            if (index < 0) {
                err = "Unknown column: " + ref;
                return false;
            }
            resolved.fromRight = true;
            resolved.index = index;
            resolved.outputName = trimCopy(ref);
            return true;
        }

        err = "Unknown column: " + ref;
        return false;
    }

    const int leftIndex = findColumnIndexCaseInsensitive(left, qualified.second);
    const int rightIndex = findColumnIndexCaseInsensitive(right, qualified.second);
    if (leftIndex >= 0 && rightIndex >= 0) {
        err = "Ambiguous column: " + ref;
        return false;
    }
    if (leftIndex >= 0) {
        resolved.fromRight = false;
        resolved.index = leftIndex;
        resolved.outputName = left.schema[leftIndex].name;
        return true;
    }
    if (rightIndex >= 0) {
        resolved.fromRight = true;
        resolved.index = rightIndex;
        resolved.outputName = right.schema[rightIndex].name;
        return true;
    }

    err = "Unknown column: " + ref;
    return false;
}

bool invokeCallback(const CachedResult &result,
                    flexql_callback callback,
                    void *arg) {
    if (!callback) {
        return false;
    }

    std::vector<char *> names(result.columnNames.size(), nullptr);
    for (size_t i = 0; i < result.columnNames.size(); ++i) {
        names[i] = const_cast<char *>(result.columnNames[i].c_str());
    }

    for (const auto &row : result.rows) {
        std::vector<char *> values(row.size(), nullptr);
        for (size_t i = 0; i < row.size(); ++i) {
            values[i] = const_cast<char *>(row[i].c_str());
        }
        if (callback(arg,
                     static_cast<int>(result.columnNames.size()),
                     values.data(),
                     names.data()) != 0) {
            return true;
        }
    }
    return false;
}

bool invokeCallbackRow(const std::vector<std::string> &columnNames,
                       const std::vector<std::string> &row,
                       flexql_callback callback,
                       void *arg) {
    if (!callback) {
        return false;
    }

    std::vector<char *> names(columnNames.size(), nullptr);
    std::vector<char *> values(row.size(), nullptr);
    for (size_t i = 0; i < columnNames.size(); ++i) {
        names[i] = const_cast<char *>(columnNames[i].c_str());
    }
    for (size_t i = 0; i < row.size(); ++i) {
        values[i] = const_cast<char *>(row[i].c_str());
    }

    return callback(arg,
                    static_cast<int>(columnNames.size()),
                    values.data(),
                    names.data()) != 0;
}

size_t rowBytes(const std::vector<std::string> &values) {
    size_t bytes = sizeof(std::vector<std::string>);
    for (const std::string &value : values) {
        bytes += sizeof(std::string) + value.size();
    }
    return bytes;
}

size_t columnNameBytes(const std::vector<std::string> &columnNames) {
    size_t bytes = sizeof(std::vector<std::string>);
    for (const std::string &name : columnNames) {
        bytes += sizeof(std::string) + name.size();
    }
    return bytes;
}

bool trackCachedRow(CachedResult &result,
                    const std::vector<std::string> &values,
                    int64_t expiresAt,
                    size_t byteLimit,
                    size_t &usedBytes) {
    const size_t needed = rowBytes(values) + sizeof(int64_t);
    if (usedBytes + needed > byteLimit) {
        return false;
    }

    result.rows.push_back(values);
    result.rowExpiresAt.push_back(expiresAt);
    usedBytes += needed;
    return true;
}

bool validateValueType(const ColumnDef &column,
                       const std::string &rawValue,
                       const std::string &value,
                       std::string &err) {
    if (isNullLiteral(rawValue)) {
        if (column.notNull) {
            err = "NULL value not allowed for column: " + column.name;
            return false;
        }
        return true;
    }

    switch (column.type) {
        case ColType::INT:
            if (!parseIntegerStrict(value)) {
                err = "Invalid INT value for column: " + column.name;
                return false;
            }
            return true;
        case ColType::DECIMAL:
            if (!parseDoubleStrict(value)) {
                err = "Invalid DECIMAL value for column: " + column.name;
                return false;
            }
            return true;
        case ColType::VARCHAR:
        case ColType::DATETIME:
            return true;
    }
    return true;
}

} // namespace

Executor::Executor(StorageEngine &storage, LRUCache &cache, DurableLog *durableLog)
    : storage_(storage),
      cache_(cache),
      durableLog_(durableLog),
      maxIndexedRows_(parseSizeEnv("FLEXQL_MAX_INDEXED_ROWS", 15000000)) {}

void Executor::registerIndex(const std::string &table, int /*pkColIdx*/) {
    std::lock_guard<std::mutex> lock(indexMutex_);
    if (indexes_.count(table) == 0) {
        indexes_[table] = std::make_unique<HashIndex>(1 << 20);
    }
}

void Executor::rebuildIndex(const std::string &table) {
    Table *tbl = storage_.getTable(table);
    if (tbl == nullptr || tbl->primaryKeyIdx < 0) {
        return;
    }

    std::shared_lock<std::shared_mutex> tableLock(tbl->rwlock);
    std::lock_guard<std::mutex> indexLock(indexMutex_);

    if (maxIndexedRows_ > 0 && storage_.rowCount(*tbl) > maxIndexedRows_) {
        indexes_.erase(table);
        return;
    }

    auto &index = indexes_[table];
    if (!index) {
        index = std::make_unique<HashIndex>(1 << 20);
    } else {
        index->clear();
    }

    std::string err;
    const bool ok = storage_.scanRows(*tbl, [&](const Row &row, uint64_t offset) {
        if (tbl->primaryKeyIdx >= 0 &&
            static_cast<size_t>(tbl->primaryKeyIdx) < row.values.size()) {
            index->insert(row.values[tbl->primaryKeyIdx], static_cast<size_t>(offset));
        }
        return true;
    }, &err);

    if (!ok) {
        indexes_.erase(table);
    }
}

int Executor::execute(const ParsedQuery &query,
                      flexql_callback callback,
                      void *arg,
                      std::string &errMsg) {
    switch (query.type) {
        case QueryType::CREATE_TABLE:
            return execCreate(query, errMsg);
        case QueryType::INSERT:
            return execInsert(query, errMsg);
        case QueryType::SELECT:
            return execSelect(query, callback, arg, errMsg);
        case QueryType::DELETE_ROWS:
            return execDelete(query, errMsg);
        default:
            errMsg = "Unknown query type";
            return FLEXQL_ERROR;
    }
}

int Executor::execCreate(const ParsedQuery &q, std::string &err) {
    std::lock_guard<std::mutex> mutationLock(mutationMutex_);
    std::unordered_set<std::string> seenColumns;
    int primaryKeyCount = 0;
    for (const ColumnDef &column : q.columns) {
        const std::string upperName = upperCopy(column.name);
        if (!seenColumns.insert(upperName).second) {
            err = "Duplicate column name: " + column.name;
            return FLEXQL_ERROR;
        }
        if (column.primaryKey) {
            ++primaryKeyCount;
        }
    }
    if (primaryKeyCount > 1) {
        err = "Only one PRIMARY KEY is supported";
        return FLEXQL_ERROR;
    }

    if (storage_.tableExists(q.tableName)) {
        if (q.createIfNotExists) {
            return FLEXQL_OK;
        }
        err = "Table already exists: " + q.tableName;
        return FLEXQL_ERROR;
    }

    uint64_t txId = 0;
    if (durableLog_ != nullptr && !durableLog_->beginCreate(q, txId, err)) {
        return FLEXQL_ERROR;
    }

    if (!storage_.createTable(q.tableName, q.columns, &err)) {
        if (err.empty()) {
            err = "Failed to create table: " + q.tableName;
        }
        return FLEXQL_ERROR;
    }

    if (durableLog_ != nullptr && !durableLog_->commit(txId, err)) {
        std::string rollbackErr;
        if (!storage_.dropTable(q.tableName, &rollbackErr) && !rollbackErr.empty()) {
            err += "; rollback failed: " + rollbackErr;
        }
        return FLEXQL_ERROR;
    }

    Table *tbl = storage_.getTable(q.tableName);
    if (tbl != nullptr && tbl->primaryKeyIdx >= 0) {
        registerIndex(q.tableName, tbl->primaryKeyIdx);
    }
    return FLEXQL_OK;
}

int Executor::execInsert(const ParsedQuery &q, std::string &err) {
    std::lock_guard<std::mutex> mutationLock(mutationMutex_);
    Table *tbl = storage_.getTable(q.tableName);
    if (tbl == nullptr) {
        err = "No such table: " + q.tableName;
        return FLEXQL_ERROR;
    }
    if (q.insertRows.empty()) {
        err = "INSERT requires at least one row";
        return FLEXQL_ERROR;
    }

    {
        std::unique_lock<std::shared_mutex> tableLock(tbl->rwlock);
        std::lock_guard<std::mutex> indexLock(indexMutex_);
        const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        if (tbl->primaryKeyIdx >= 0 &&
            maxIndexedRows_ > 0 &&
            storage_.rowCount(*tbl) + q.insertRows.size() > maxIndexedRows_) {
            indexes_.erase(q.tableName);
        }

        HashIndex *index = nullptr;
        const auto indexIt = indexes_.find(q.tableName);
        if (indexIt != indexes_.end()) {
            index = indexIt->second.get();
        }

        std::unordered_set<std::string> batchPrimaryKeys;
        std::unordered_set<std::string> existingPrimaryKeys;
        bool loadedExistingPrimaryKeys = false;
        auto loadExistingPrimaryKeys = [&]() -> bool {
            if (loadedExistingPrimaryKeys) {
                return true;
            }
            std::string scanErr;
            const bool scanOk = storage_.scanRows(*tbl, [&](const Row &row, uint64_t) {
                if (tbl->primaryKeyIdx >= 0 &&
                    static_cast<size_t>(tbl->primaryKeyIdx) < row.values.size() &&
                    !rowExpired(row, nowMs)) {
                    existingPrimaryKeys.insert(row.values[tbl->primaryKeyIdx]);
                }
                return true;
            }, &scanErr);
            if (!scanOk) {
                err = scanErr.empty() ? "Failed to scan table for duplicate keys" : scanErr;
                return false;
            }
            loadedExistingPrimaryKeys = true;
            return true;
        };

        uint64_t startingOffset = 0;
        const uint64_t startingRowCount = storage_.rowCount(*tbl);
        if (!storage_.getRowFileEndOffset(*tbl, startingOffset, &err)) {
            return FLEXQL_ERROR;
        }

        auto rollbackInsert = [&]() {
            std::string rollbackErr;
            if (!storage_.truncateTable(*tbl, startingOffset, startingRowCount, &rollbackErr) &&
                !rollbackErr.empty()) {
                if (!err.empty()) {
                    err += "; ";
                }
                err += "rollback failed: " + rollbackErr;
            }
            if (index != nullptr && tbl->primaryKeyIdx >= 0) {
                for (const std::vector<std::string> &values : q.insertRows) {
                    if (static_cast<size_t>(tbl->primaryKeyIdx) < values.size()) {
                        index->remove(values[tbl->primaryKeyIdx]);
                    }
                }
            }
        };

        /* --- Validation pass: check types and duplicate keys --- */
        for (size_t rowNo = 0; rowNo < q.insertRows.size(); ++rowNo) {
            const std::vector<std::string> &values = q.insertRows[rowNo];
            const std::vector<std::string> &rawValues =
                (rowNo < q.insertRawRows.size()) ? q.insertRawRows[rowNo] : q.insertRows[rowNo];

            if (values.size() != tbl->schema.size()) {
                err = "Column count mismatch";
                return FLEXQL_ERROR;
            }

            for (size_t colNo = 0; colNo < values.size(); ++colNo) {
                if (!validateValueType(tbl->schema[colNo], rawValues[colNo], values[colNo], err)) {
                    return FLEXQL_ERROR;
                }
            }

            if (tbl->primaryKeyIdx >= 0) {
                const std::string &primaryKey = values[tbl->primaryKeyIdx];
                if (!batchPrimaryKeys.insert(primaryKey).second) {
                    err = "Duplicate PRIMARY KEY value in INSERT batch";
                    return FLEXQL_ERROR;
                }

                if (index != nullptr) {
                    const size_t existing = index->lookup(primaryKey);
                    if (existing != SIZE_MAX) {
                        Row existingRow;
                        std::string readErr;
                        if (storage_.readRow(*tbl, static_cast<uint64_t>(existing), existingRow, &readErr) &&
                            !rowExpired(existingRow, nowMs)) {
                            err = "Duplicate PRIMARY KEY value";
                            return FLEXQL_ERROR;
                        }
                        if (!readErr.empty() && existingRow.values.empty()) {
                            err = readErr;
                            return FLEXQL_ERROR;
                        }
                    }
                } else {
                    /* No in-memory index — fall back to full scan for PK check */
                    if (!loadExistingPrimaryKeys()) {
                        return FLEXQL_ERROR;
                    }
                    if (existingPrimaryKeys.count(primaryKey) != 0) {
                        err = "Duplicate PRIMARY KEY value";
                        return FLEXQL_ERROR;
                    }
                    existingPrimaryKeys.insert(primaryKey);
                }
            }
        }

        /* --- WAL prepare (no fsync yet) --- */
        uint64_t txId = 0;
        if (durableLog_ != nullptr && !durableLog_->beginInsertNoSync(q, txId, err)) {
            return FLEXQL_ERROR;
        }

        /* --- Batch write all rows in one write() call --- */
        std::vector<uint64_t> rowOffsets;
        if (!storage_.insertRowsBatch(*tbl, q.insertRows, q.expiresAt, &rowOffsets, &err)) {
            if (err.empty()) {
                err = "Insert failed";
            }
            rollbackInsert();
            return FLEXQL_ERROR;
        }

        /* --- Update in-memory index from batch offsets --- */
        if (tbl->primaryKeyIdx >= 0 && index != nullptr) {
            for (size_t i = 0; i < q.insertRows.size(); ++i) {
                index->insert(q.insertRows[i][tbl->primaryKeyIdx],
                              static_cast<size_t>(rowOffsets[i]));
            }
        }

        /* --- WAL commit (no fsync yet) --- */
        if (durableLog_ != nullptr && !durableLog_->commitNoSync(txId, err)) {
            rollbackInsert();
            return FLEXQL_ERROR;
        }

        /* --- Single fsync for both prepare + commit --- */
        if (durableLog_ != nullptr && !durableLog_->sync(err)) {
            rollbackInsert();
            return FLEXQL_ERROR;
        }

        /* --- Update disk primary key index (best-effort, non-critical) --- */
        if (tbl->primaryKeyIdx >= 0 && storage_.hasPrimaryKeyIndex(*tbl)) {
            for (size_t i = 0; i < q.insertRows.size(); ++i) {
                storage_.upsertPrimaryKey(*tbl, q.insertRows[i][tbl->primaryKeyIdx], rowOffsets[i]);
            }
        }
    }

    cache_.invalidateTable(q.tableName);
    return FLEXQL_OK;
}

int Executor::execDelete(const ParsedQuery &q, std::string &err) {
    std::lock_guard<std::mutex> mutationLock(mutationMutex_);
    Table *tbl = storage_.getTable(q.tableName);
    if (tbl == nullptr) {
        err = "No such table: " + q.tableName;
        return FLEXQL_ERROR;
    }

    const uint64_t originalRowCount = storage_.rowCount(*tbl);
    std::string backupPath;
    if (originalRowCount > 0) {
        backupPath = tbl->rowFilePath + ".rollback." +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::error_code ec;
        fs::copy_file(tbl->rowFilePath, backupPath, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            err = "Failed to snapshot table before DELETE";
            return FLEXQL_ERROR;
        }
    }

    uint64_t txId = 0;
    if (durableLog_ != nullptr && !durableLog_->beginDelete(q, txId, err)) {
        if (!backupPath.empty()) {
            std::error_code ec;
            fs::remove(backupPath, ec);
        }
        return FLEXQL_ERROR;
    }

    {
        std::unique_lock<std::shared_mutex> tableLock(tbl->rwlock);
        auto rollbackDelete = [&]() {
            if (backupPath.empty()) {
                return;
            }
            std::string rollbackErr;
            if (!storage_.restoreTableFromBackup(*tbl, backupPath, originalRowCount, &rollbackErr) &&
                !rollbackErr.empty()) {
                if (!err.empty()) {
                    err += "; ";
                }
                err += "rollback failed: " + rollbackErr;
            }
        };

        if (!storage_.clearTable(*tbl, &err)) {
            if (err.empty()) {
                err = "Failed to clear table";
            }
            rollbackDelete();
            return FLEXQL_ERROR;
        }

        if (durableLog_ != nullptr && !durableLog_->commit(txId, err)) {
            rollbackDelete();
            return FLEXQL_ERROR;
        }

        std::lock_guard<std::mutex> indexLock(indexMutex_);
        const auto it = indexes_.find(q.tableName);
        if (it != indexes_.end()) {
            it->second->clear();
        }
    }

    if (!backupPath.empty()) {
        std::error_code ec;
        fs::remove(backupPath, ec);
    }

    cache_.invalidateTable(q.tableName);
    return FLEXQL_OK;
}

std::string Executor::makeCacheKey(const ParsedQuery &q) {
    std::ostringstream out;
    out << q.tableName;
    if (q.join.active) {
        out << "|JOIN|" << q.join.rightTable << "|" << q.join.leftCol
            << q.join.op << q.join.rightCol;
    }
    out << "|SELECT|";
    for (const std::string &column : q.selectCols) {
        out << column << ",";
    }
    if (q.where.active) {
        out << "|WHERE|" << q.where.column << q.where.op << q.where.value;
    }
    if (q.orderBy.active) {
        out << "|ORDER|" << q.orderBy.column << (q.orderBy.ascending ? "ASC" : "DESC");
    }
    return out.str();
}

int Executor::execSelect(const ParsedQuery &q,
                         flexql_callback callback,
                         void *arg,
                         std::string &err) {
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string cacheKey = makeCacheKey(q);
    CachedResult cached;
    if (cache_.get(cacheKey, cached)) {
        if (filterExpiredCachedRows(cached, nowMs)) {
            cache_.put(cacheKey, cached);
        }
        invokeCallback(cached, callback, arg);
        return FLEXQL_OK;
    }

    Table *left = storage_.getTable(q.tableName);
    if (left == nullptr) {
        err = "No such table: " + q.tableName;
        return FLEXQL_ERROR;
    }

    auto buildPlainProjection = [&](const Table &table,
                                    std::vector<ResolvedColumn> &projection,
                                    std::vector<std::string> &columnNames) -> bool {
        if (q.selectCols.empty()) {
            projection.reserve(table.schema.size());
            columnNames.reserve(table.schema.size());
            for (size_t i = 0; i < table.schema.size(); ++i) {
                ResolvedColumn col;
                col.index = static_cast<int>(i);
                col.outputName = table.schema[i].name;
                projection.push_back(col);
                columnNames.push_back(col.outputName);
            }
            return true;
        }

        projection.reserve(q.selectCols.size());
        columnNames.reserve(q.selectCols.size());
        for (const std::string &columnRef : q.selectCols) {
            ResolvedColumn col;
            if (!resolvePlainColumn(table, q.tableName, columnRef, col, err)) {
                return false;
            }
            projection.push_back(col);
            columnNames.push_back(col.outputName);
        }
        return true;
    };

    auto resolvePlainFilter = [&](const Table &table, ResolvedFilter &filter) -> bool {
        if (!q.where.active) {
            return true;
        }

        ResolvedColumn whereColumn;
        if (!resolvePlainColumn(table, q.tableName, q.where.column, whereColumn, err)) {
            return false;
        }
        filter.active = true;
        filter.index = whereColumn.index;
        filter.op = q.where.op;
        filter.value = q.where.value;
        return true;
    };

    auto emitResultStream = [&](const std::vector<std::string> &columnNames,
                                const std::function<bool(const std::function<bool(const std::vector<std::string> &, int64_t)> &)> &producer)
        -> int {
        CachedResult result;
        result.columnNames = columnNames;
        size_t usedBytes = columnNameBytes(result.columnNames);
        bool cacheable = cache_.byteCapacity() > 0;
        bool aborted = false;

        const bool produced = producer([&](const std::vector<std::string> &values, int64_t expiresAt) {
            if (invokeCallbackRow(result.columnNames, values, callback, arg)) {
                aborted = true;
                return false;
            }
            if (cacheable && !trackCachedRow(result, values, expiresAt, cache_.byteCapacity(), usedBytes)) {
                cacheable = false;
                result.rows.clear();
                result.rowExpiresAt.clear();
            }
            return true;
        });

        if (!produced) {
            return FLEXQL_ERROR;
        }

        if (!aborted && cacheable) {
            cache_.put(cacheKey, result);
        }
        return FLEXQL_OK;
    };

    auto tryLoadPrimaryKeyRow = [&](Table &tbl,
                                    const std::string &tableName,
                                    const std::string &key,
                                    Row &row,
                                    uint64_t &rowOffset,
                                    bool &usedFastPath) -> bool {
        usedFastPath = false;
        rowOffset = UINT64_MAX;

        {
            std::lock_guard<std::mutex> indexLock(indexMutex_);
            const auto idxIt = indexes_.find(tableName);
            if (idxIt != indexes_.end()) {
                usedFastPath = true;
                const size_t indexedOffset = idxIt->second->lookup(key);
                rowOffset = (indexedOffset == SIZE_MAX) ? UINT64_MAX
                                                        : static_cast<uint64_t>(indexedOffset);
            }
        }

        if (usedFastPath) {
            if (rowOffset == UINT64_MAX) {
                return true;
            }
            std::string readErr;
            if (!storage_.readRow(tbl, rowOffset, row, &readErr)) {
                err = readErr.empty() ? "Failed to read indexed row" : readErr;
                return false;
            }
            return true;
        }

        if (storage_.hasPrimaryKeyIndex(tbl)) {
            std::string lookupErr;
            if (storage_.lookupPrimaryKey(tbl, key, rowOffset, &row, &lookupErr)) {
                usedFastPath = true;
            }
        }

        return true;
    };

    if (!q.join.active) {
        std::shared_lock<std::shared_mutex> leftLock(left->rwlock);

        std::vector<ResolvedColumn> projection;
        std::vector<std::string> columnNames;
        if (!buildPlainProjection(*left, projection, columnNames)) {
            return FLEXQL_ERROR;
        }

        ResolvedFilter filter;
        if (!resolvePlainFilter(*left, filter)) {
            return FLEXQL_ERROR;
        }

        ResolvedSort sort;
        if (q.orderBy.active) {
            if (!resolvePlainColumn(*left, q.tableName, q.orderBy.column, sort.column, err)) {
                return FLEXQL_ERROR;
            }
            sort.active = true;
            sort.ascending = q.orderBy.ascending;
        }

        if (!sort.active) {
            return emitResultStream(columnNames, [&](const auto &emitRow) {
                uint64_t indexedOffset = UINT64_MAX;
                Row indexedRow;
                bool usedFastPath = false;
                if (filter.active && filter.op == "=" && left->primaryKeyIdx >= 0 &&
                    filter.index == left->primaryKeyIdx) {
                    if (!tryLoadPrimaryKeyRow(*left, q.tableName, filter.value,
                                              indexedRow, indexedOffset, usedFastPath)) {
                        return false;
                    }
                }

                if (usedFastPath) {
                    if (indexedOffset == UINT64_MAX) {
                        return true;
                    }
                    if (rowExpired(indexedRow, nowMs)) {
                        return true;
                    }

                    std::vector<std::string> projected;
                    projected.reserve(projection.size());
                    for (const ResolvedColumn &col : projection) {
                        projected.push_back(indexedRow.values[col.index]);
                    }
                    return emitRow(projected, indexedRow.expires_at);
                }

                std::string scanErr;
                const bool scanOk = storage_.scanRows(*left, [&](const Row &row, uint64_t) {
                    if (rowExpired(row, nowMs)) {
                        return true;
                    }
                    if (filter.active &&
                        !compareWithOperator(row.values[filter.index], filter.op, filter.value)) {
                        return true;
                    }

                    std::vector<std::string> projected;
                    projected.reserve(projection.size());
                    for (const ResolvedColumn &col : projection) {
                        projected.push_back(row.values[col.index]);
                    }
                    return emitRow(projected, row.expires_at);
                }, &scanErr);

                if (!scanOk) {
                    err = scanErr.empty() ? "Failed to scan table" : scanErr;
                }
                return scanOk;
            });
        }

        std::vector<ResultRow> rows;
        uint64_t indexedOffset = UINT64_MAX;
        Row indexedRow;
        bool usedFastPath = false;
        if (filter.active && filter.op == "=" && left->primaryKeyIdx >= 0 &&
            filter.index == left->primaryKeyIdx) {
            if (!tryLoadPrimaryKeyRow(*left, q.tableName, filter.value,
                                      indexedRow, indexedOffset, usedFastPath)) {
                return FLEXQL_ERROR;
            }
        }

        if (usedFastPath) {
            if (indexedOffset != UINT64_MAX && !rowExpired(indexedRow, nowMs)) {
                    ResultRow resultRow;
                    resultRow.values.reserve(projection.size());
                    for (const ResolvedColumn &col : projection) {
                        resultRow.values.push_back(indexedRow.values[col.index]);
                    }
                    resultRow.expiresAt = indexedRow.expires_at;
                    resultRow.sortValue = indexedRow.values[sort.column.index];
                    rows.push_back(std::move(resultRow));
            }
        } else {
            std::string scanErr;
            const bool scanOk = storage_.scanRows(*left, [&](const Row &row, uint64_t) {
                if (rowExpired(row, nowMs)) {
                    return true;
                }
                if (filter.active &&
                    !compareWithOperator(row.values[filter.index], filter.op, filter.value)) {
                    return true;
                }

                ResultRow resultRow;
                resultRow.values.reserve(projection.size());
                for (const ResolvedColumn &col : projection) {
                    resultRow.values.push_back(row.values[col.index]);
                }
                resultRow.expiresAt = row.expires_at;
                resultRow.sortValue = row.values[sort.column.index];
                rows.push_back(std::move(resultRow));
                return true;
            }, &scanErr);
            if (!scanOk) {
                err = scanErr.empty() ? "Failed to scan table" : scanErr;
                return FLEXQL_ERROR;
            }
        }

        std::sort(rows.begin(), rows.end(), [&](const ResultRow &lhs, const ResultRow &rhs) {
            const int cmp = compareValues(lhs.sortValue, rhs.sortValue);
            return sort.ascending ? (cmp < 0) : (cmp > 0);
        });

        CachedResult result;
        result.columnNames = columnNames;
        result.rows.reserve(rows.size());
        result.rowExpiresAt.reserve(rows.size());
        for (const ResultRow &row : rows) {
            result.rows.push_back(row.values);
            result.rowExpiresAt.push_back(row.expiresAt);
        }

        const bool aborted = invokeCallback(result, callback, arg);
        if (!aborted) {
            cache_.put(cacheKey, result);
        }
        return FLEXQL_OK;
    }

    Table *right = storage_.getTable(q.join.rightTable);
    if (right == nullptr) {
        err = "No such table: " + q.join.rightTable;
        return FLEXQL_ERROR;
    }

    std::shared_lock<std::shared_mutex> leftLock(left->rwlock);
    std::shared_lock<std::shared_mutex> rightLock(right->rwlock);

    ResolvedColumn leftJoin;
    ResolvedColumn rightJoin;
    if (!resolvePlainColumn(*left, q.tableName, q.join.leftCol, leftJoin, err)) {
        return FLEXQL_ERROR;
    }
    if (!resolvePlainColumn(*right, q.join.rightTable, q.join.rightCol, rightJoin, err)) {
        return FLEXQL_ERROR;
    }

    std::vector<ResolvedColumn> projection;
    std::vector<std::string> columnNames;
    if (q.selectCols.empty()) {
        projection.reserve(left->schema.size() + right->schema.size());
        columnNames.reserve(left->schema.size() + right->schema.size());
        for (size_t i = 0; i < left->schema.size(); ++i) {
            ResolvedColumn col;
            col.fromRight = false;
            col.index = static_cast<int>(i);
            col.outputName = q.tableName + "." + left->schema[i].name;
            projection.push_back(col);
            columnNames.push_back(col.outputName);
        }
        for (size_t i = 0; i < right->schema.size(); ++i) {
            ResolvedColumn col;
            col.fromRight = true;
            col.index = static_cast<int>(i);
            col.outputName = q.join.rightTable + "." + right->schema[i].name;
            projection.push_back(col);
            columnNames.push_back(col.outputName);
        }
    } else {
        projection.reserve(q.selectCols.size());
        columnNames.reserve(q.selectCols.size());
        for (const std::string &columnRef : q.selectCols) {
            ResolvedColumn col;
            if (!resolveJoinColumn(*left, *right, q.tableName, q.join.rightTable,
                                   columnRef, col, err)) {
                return FLEXQL_ERROR;
            }
            projection.push_back(col);
            columnNames.push_back(col.outputName);
        }
    }

    ResolvedFilter filter;
    if (q.where.active) {
        ResolvedColumn whereColumn;
        if (!resolveJoinColumn(*left, *right, q.tableName, q.join.rightTable,
                               q.where.column, whereColumn, err)) {
            return FLEXQL_ERROR;
        }
        filter.active = true;
        filter.fromRight = whereColumn.fromRight;
        filter.index = whereColumn.index;
        filter.op = q.where.op;
        filter.value = q.where.value;
    }

    ResolvedSort sort;
    if (q.orderBy.active) {
        if (!resolveJoinColumn(*left, *right, q.tableName, q.join.rightTable,
                               q.orderBy.column, sort.column, err)) {
            return FLEXQL_ERROR;
        }
        sort.active = true;
        sort.ascending = q.orderBy.ascending;
    }

    bool canUseRightIndex = (q.join.op == "=" && right->primaryKeyIdx == rightJoin.index);

    auto projectJoinedRow = [&](const Row &leftRow, const Row &rightRow, ResultRow *sortedRow)
        -> std::vector<std::string> {
        std::vector<std::string> values;
        values.reserve(projection.size());
        for (const ResolvedColumn &col : projection) {
            values.push_back(col.fromRight
                ? rightRow.values[col.index]
                : leftRow.values[col.index]);
        }

        if (sortedRow != nullptr) {
            sortedRow->values = values;
            sortedRow->expiresAt = combineExpiry(leftRow.expires_at, rightRow.expires_at);
            if (sort.active) {
                sortedRow->sortValue = sort.column.fromRight
                    ? rightRow.values[sort.column.index]
                    : leftRow.values[sort.column.index];
            }
        }
        return values;
    };

    auto joinMatches = [&](const Row &leftRow, const Row &rightRow) {
        if (!compareWithOperator(leftRow.values[leftJoin.index], q.join.op,
                                 rightRow.values[rightJoin.index])) {
            return false;
        }
        if (!filter.active) {
            return true;
        }
        const std::string &candidate =
            filter.fromRight ? rightRow.values[filter.index] : leftRow.values[filter.index];
        return compareWithOperator(candidate, filter.op, filter.value);
    };

    if (!sort.active) {
        return emitResultStream(columnNames, [&](const auto &emitRow) {
            std::string leftScanErr;
            const bool leftOk = storage_.scanRows(*left, [&](const Row &leftRow, uint64_t) {
                if (rowExpired(leftRow, nowMs)) {
                    return true;
                }

                bool usedRightIndex = false;
                uint64_t rightOffset = UINT64_MAX;
                Row rightRow;
                if (canUseRightIndex) {
                    if (!tryLoadPrimaryKeyRow(*right, q.join.rightTable, leftRow.values[leftJoin.index],
                                              rightRow, rightOffset, usedRightIndex)) {
                        return false;
                    }
                }

                if (usedRightIndex) {
                    if (rightOffset == UINT64_MAX) {
                        return true;
                    }
                    if (rowExpired(rightRow, nowMs) || !joinMatches(leftRow, rightRow)) {
                        return true;
                    }

                    const std::vector<std::string> values = projectJoinedRow(leftRow, rightRow, nullptr);
                    return emitRow(values, combineExpiry(leftRow.expires_at, rightRow.expires_at));
                }

                std::string rightScanErr;
                const bool rightOk = storage_.scanRows(*right, [&](const Row &rightRow, uint64_t) {
                    if (rowExpired(rightRow, nowMs) || !joinMatches(leftRow, rightRow)) {
                        return true;
                    }

                    const std::vector<std::string> values = projectJoinedRow(leftRow, rightRow, nullptr);
                    return emitRow(values, combineExpiry(leftRow.expires_at, rightRow.expires_at));
                }, &rightScanErr);

                if (!rightOk) {
                    err = rightScanErr.empty() ? "Failed to scan joined table" : rightScanErr;
                }
                return rightOk;
            }, &leftScanErr);

            if (!leftOk && err.empty()) {
                err = leftScanErr.empty() ? "Failed to scan left table" : leftScanErr;
            }
            return leftOk;
        });
    }

    std::vector<ResultRow> rows;
    std::string leftScanErr;
    const bool leftOk = storage_.scanRows(*left, [&](const Row &leftRow, uint64_t) {
        if (rowExpired(leftRow, nowMs)) {
            return true;
        }

        bool usedRightIndex = false;
        uint64_t rightOffset = UINT64_MAX;
        Row rightRow;
        if (canUseRightIndex) {
            if (!tryLoadPrimaryKeyRow(*right, q.join.rightTable, leftRow.values[leftJoin.index],
                                      rightRow, rightOffset, usedRightIndex)) {
                return false;
            }
        }

        if (usedRightIndex) {
            if (rightOffset == UINT64_MAX) {
                return true;
            }
            if (rowExpired(rightRow, nowMs) || !joinMatches(leftRow, rightRow)) {
                return true;
            }

            ResultRow resultRow;
            projectJoinedRow(leftRow, rightRow, &resultRow);
            rows.push_back(std::move(resultRow));
            return true;
        }

        std::string rightScanErr;
        const bool rightOk = storage_.scanRows(*right, [&](const Row &rightRow, uint64_t) {
            if (rowExpired(rightRow, nowMs) || !joinMatches(leftRow, rightRow)) {
                return true;
            }

            ResultRow resultRow;
            projectJoinedRow(leftRow, rightRow, &resultRow);
            rows.push_back(std::move(resultRow));
            return true;
        }, &rightScanErr);

        if (!rightOk) {
            err = rightScanErr.empty() ? "Failed to scan joined table" : rightScanErr;
        }
        return rightOk;
    }, &leftScanErr);

    if (!leftOk) {
        if (err.empty()) {
            err = leftScanErr.empty() ? "Failed to scan left table" : leftScanErr;
        }
        return FLEXQL_ERROR;
    }

    std::sort(rows.begin(), rows.end(), [&](const ResultRow &lhs, const ResultRow &rhs) {
        const int cmp = compareValues(lhs.sortValue, rhs.sortValue);
        return sort.ascending ? (cmp < 0) : (cmp > 0);
    });

    CachedResult result;
    result.columnNames = columnNames;
    result.rows.reserve(rows.size());
    result.rowExpiresAt.reserve(rows.size());
    for (const ResultRow &row : rows) {
        result.rows.push_back(row.values);
        result.rowExpiresAt.push_back(row.expiresAt);
    }

    const bool aborted = invokeCallback(result, callback, arg);
    if (!aborted) {
        cache_.put(cacheKey, result);
    }
    return FLEXQL_OK;
}
