/*
 * FlexQL: Binary snapshot persistence implementation.
 */

#include "persistence.h"

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace {

constexpr char kSnapshotMagic[] = "FXQLSNAP";
constexpr uint32_t kSnapshotVersion = 1;

template <typename T>
bool writePod(std::ofstream &out, const T &value) {
    out.write(reinterpret_cast<const char *>(&value), sizeof(T));
    return static_cast<bool>(out);
}

template <typename T>
bool readPod(std::ifstream &in, T &value) {
    in.read(reinterpret_cast<char *>(&value), sizeof(T));
    return static_cast<bool>(in);
}

bool writeString(std::ofstream &out, const std::string &value) {
    const uint32_t size = static_cast<uint32_t>(value.size());
    return writePod(out, size) &&
           (size == 0 || static_cast<bool>(out.write(value.data(), size)));
}

bool readString(std::ifstream &in, std::string &value) {
    uint32_t size = 0;
    if (!readPod(in, size)) {
        return false;
    }
    value.resize(size);
    return size == 0 || static_cast<bool>(in.read(&value[0], size));
}

bool ensureParentDir(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return true;
    }

    const std::string dir = path.substr(0, slash);
    if (dir.empty()) {
        return true;
    }

    if (::mkdir(dir.c_str(), 0777) == 0) {
        return true;
    }
    return errno == EEXIST;
}

bool rowExpired(const Row &row, int64_t nowMs) {
    return row.expires_at != 0 && row.expires_at <= nowMs;
}

} // namespace

namespace persistence {

bool loadSnapshot(const std::string &path,
                  StorageEngine &storage,
                  Executor &executor,
                  std::string &err) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return true;
    }

    char magic[sizeof(kSnapshotMagic)] = {};
    in.read(magic, sizeof(magic) - 1);
    if (!in || std::strncmp(magic, kSnapshotMagic, sizeof(kSnapshotMagic) - 1) != 0) {
        err = "Invalid snapshot header";
        return false;
    }

    uint32_t version = 0;
    if (!readPod(in, version) || version != kSnapshotVersion) {
        err = "Unsupported snapshot version";
        return false;
    }

    uint32_t tableCount = 0;
    if (!readPod(in, tableCount)) {
        err = "Failed to read snapshot table count";
        return false;
    }

    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (uint32_t tableNo = 0; tableNo < tableCount; ++tableNo) {
        std::string tableName;
        if (!readString(in, tableName)) {
            err = "Failed to read table name from snapshot";
            return false;
        }

        uint32_t columnCount = 0;
        if (!readPod(in, columnCount)) {
            err = "Failed to read schema size from snapshot";
            return false;
        }

        std::vector<ColumnDef> schema;
        schema.reserve(columnCount);
        for (uint32_t colNo = 0; colNo < columnCount; ++colNo) {
            ColumnDef column;
            uint32_t typeValue = 0;
            uint8_t notNull = 0;
            uint8_t primaryKey = 0;

            if (!readString(in, column.name) ||
                !readPod(in, typeValue) ||
                !readPod(in, notNull) ||
                !readPod(in, primaryKey)) {
                err = "Failed to read column definition from snapshot";
                return false;
            }

            column.type = static_cast<ColType>(typeValue);
            column.notNull = (notNull != 0);
            column.primaryKey = (primaryKey != 0);
            schema.push_back(column);
        }

        if (!storage.createTable(tableName, schema)) {
            err = "Failed to restore table: " + tableName;
            return false;
        }

        uint64_t rowCount = 0;
        if (!readPod(in, rowCount)) {
            err = "Failed to read row count from snapshot";
            return false;
        }

        Table *table = storage.getTable(tableName);
        if (table == nullptr) {
            err = "Restored table missing from storage: " + tableName;
            return false;
        }

        for (uint64_t rowNo = 0; rowNo < rowCount; ++rowNo) {
            Row row;
            uint32_t valueCount = 0;
            if (!readPod(in, row.expires_at) || !readPod(in, valueCount)) {
                err = "Failed to read row header from snapshot";
                return false;
            }

            row.values.reserve(valueCount);
            for (uint32_t valueNo = 0; valueNo < valueCount; ++valueNo) {
                std::string value;
                if (!readString(in, value)) {
                    err = "Failed to read row value from snapshot";
                    return false;
                }
                row.values.push_back(std::move(value));
            }

            if (rowExpired(row, nowMs)) {
                continue;
            }
            uint64_t rowOffset = 0;
            if (!storage.insertRow(*table, row, &rowOffset)) {
                err = "Failed to restore row for table: " + tableName;
                return false;
            }
            if (table->primaryKeyIdx >= 0) {
                storage.upsertPrimaryKey(*table, row.values[table->primaryKeyIdx], rowOffset);
            }
        }

        executor.rebuildIndex(tableName);
    }

    return true;
}

bool saveSnapshot(const std::string &path,
                  const StorageEngine &storage,
                  std::string &err) {
    if (!ensureParentDir(path)) {
        err = "Failed to create snapshot directory";
        return false;
    }

    const std::string tempPath = path + ".tmp";
    std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        err = "Failed to open snapshot for writing";
        return false;
    }

    if (!static_cast<bool>(out.write(kSnapshotMagic, sizeof(kSnapshotMagic) - 1)) ||
        !writePod(out, kSnapshotVersion)) {
        err = "Failed to write snapshot header";
        return false;
    }

    const std::vector<std::string> tableNames = storage.listTables();
    const uint32_t tableCount = static_cast<uint32_t>(tableNames.size());
    if (!writePod(out, tableCount)) {
        err = "Failed to write snapshot table count";
        return false;
    }

    for (const std::string &tableName : tableNames) {
        const Table *table = storage.getTable(tableName);
        if (table == nullptr) {
            err = "Snapshot table disappeared during save: " + tableName;
            return false;
        }

        std::shared_lock<std::shared_mutex> tableLock(table->rwlock);
        if (!writeString(out, table->name)) {
            err = "Failed to write table name";
            return false;
        }

        const uint32_t columnCount = static_cast<uint32_t>(table->schema.size());
        if (!writePod(out, columnCount)) {
            err = "Failed to write schema size";
            return false;
        }

        for (const ColumnDef &column : table->schema) {
            const uint32_t typeValue = static_cast<uint32_t>(column.type);
            const uint8_t notNull = column.notNull ? 1 : 0;
            const uint8_t primaryKey = column.primaryKey ? 1 : 0;
            if (!writeString(out, column.name) ||
                !writePod(out, typeValue) ||
                !writePod(out, notNull) ||
                !writePod(out, primaryKey)) {
                err = "Failed to write column definition";
                return false;
            }
        }

        const uint64_t rowCount = storage.rowCount(*table);
        if (!writePod(out, rowCount)) {
            err = "Failed to write row count";
            return false;
        }

        bool scanOk = storage.scanRows(*table, [&](const Row &row, uint64_t) {
            const uint32_t valueCount = static_cast<uint32_t>(row.values.size());
            if (!writePod(out, row.expires_at) || !writePod(out, valueCount)) {
                return false;
            }
            for (const std::string &value : row.values) {
                if (!writeString(out, value)) {
                    return false;
                }
            }
            return true;
        }, &err);
        if (!scanOk || !out) {
            if (err.empty()) {
                err = "Failed to stream table rows into snapshot";
            }
            return false;
        }
    }

    out.flush();
    out.close();
    if (!out) {
        err = "Failed to flush snapshot to disk";
        return false;
    }

    if (std::rename(tempPath.c_str(), path.c_str()) != 0) {
        err = "Failed to replace snapshot file";
        return false;
    }

    return true;
}

} // namespace persistence
