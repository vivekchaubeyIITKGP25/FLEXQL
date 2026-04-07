/*
 * FlexQL: Storage engine interface.
 */

#ifndef FLEXQL_STORAGE_H
#define FLEXQL_STORAGE_H

#include "types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct Table {
    std::string               name;
    std::vector<ColumnDef>    schema;
    std::string               rowFilePath;
    uint64_t                  rowCount = 0;
    int                       rowFileFd = -1;
    int                       readFileFd = -1;
    uint64_t                  rowFileEndOffset = 0;
    std::string               primaryKeyIndexPath;
    int                       primaryKeyIndexFd = -1;
    uint64_t                  primaryKeyIndexCapacity = 0;
    uint64_t                  primaryKeyIndexEntries = 0;
    bool                      primaryKeyIndexAvailable = false;
    int                       primaryKeyIdx = -1;
    mutable std::shared_mutex rwlock;

    Table() = default;
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;
};

class StorageEngine {
public:
    using RowVisitor = std::function<bool(const Row &, uint64_t)>;

    explicit StorageEngine(std::string rootPath = "");
    ~StorageEngine();

    bool createTable(const std::string &name,
                     const std::vector<ColumnDef> &schema,
                     std::string *err = nullptr);
    bool dropTable(const std::string &name, std::string *err = nullptr);
    bool tableExists(const std::string &name) const;
    Table *getTable(const std::string &name);
    const Table *getTable(const std::string &name) const;
    std::vector<std::string> listTables() const;
    bool getRowFileEndOffset(Table &tbl,
                             uint64_t &offset,
                             std::string *err = nullptr);
    bool truncateTable(Table &tbl,
                       uint64_t endOffset,
                       uint64_t rowCount,
                       std::string *err = nullptr);
    bool rebuildPrimaryKeyIndex(Table &tbl, std::string *err = nullptr);
    bool restoreTableFromBackup(Table &tbl,
                                const std::string &backupPath,
                                uint64_t rowCount,
                                std::string *err = nullptr);

    bool insertRow(Table &tbl,
                   Row row,
                   uint64_t *offset = nullptr,
                   std::string *err = nullptr);
    bool insertRowsBatch(Table &tbl,
                         const std::vector<std::vector<std::string>> &rows,
                         int64_t expiresAt,
                         std::vector<uint64_t> *offsets = nullptr,
                         std::string *err = nullptr);
    bool readRow(const Table &tbl,
                 uint64_t offset,
                 Row &row,
                 std::string *err = nullptr) const;
    bool scanRows(const Table &tbl,
                  const RowVisitor &visitor,
                  std::string *err = nullptr) const;
    bool clearTable(Table &tbl, std::string *err = nullptr);
    bool expireRows(Table &tbl, std::string *err = nullptr);
    bool lookupPrimaryKey(Table &tbl,
                          const std::string &key,
                          uint64_t &rowOffset,
                          Row *row = nullptr,
                          std::string *err = nullptr);
    bool upsertPrimaryKey(Table &tbl,
                          const std::string &key,
                          uint64_t rowOffset,
                          std::string *err = nullptr);
    bool hasPrimaryKeyIndex(const Table &tbl) const;
    uint64_t rowCount(const Table &tbl) const;

    int colIndex(const Table &tbl, const std::string &colName) const;

private:
    mutable std::shared_mutex catalogLock_;
    std::unordered_map<std::string, std::unique_ptr<Table>> tables_;
    std::string storageRoot_;
    bool ownsStorageRoot_ = false;
};

#endif /* FLEXQL_STORAGE_H */
