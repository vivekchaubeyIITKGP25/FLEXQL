/*
 * FlexQL: Query executor.
 */

#ifndef FLEXQL_EXECUTOR_H
#define FLEXQL_EXECUTOR_H

#include "flexql.h"
#include "hash_index.h"
#include "lru_cache.h"
#include "parser.h"
#include "storage.h"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

class DurableLog;

class Executor {
public:
    Executor(StorageEngine &storage, LRUCache &cache, DurableLog *durableLog = nullptr);

    int execute(const ParsedQuery &query,
                flexql_callback callback,
                void *arg,
                std::string &errMsg);

    void registerIndex(const std::string &table, int pkColIdx);
    void rebuildIndex(const std::string &table);

private:
    StorageEngine &storage_;
    LRUCache      &cache_;
    DurableLog    *durableLog_ = nullptr;

    std::unordered_map<std::string, std::unique_ptr<HashIndex>> indexes_;
    std::mutex indexMutex_;
    std::mutex mutationMutex_;
    size_t maxIndexedRows_ = 0;

    int execCreate(const ParsedQuery &q, std::string &err);
    int execInsert(const ParsedQuery &q, std::string &err);
    int execSelect(const ParsedQuery &q,
                   flexql_callback callback,
                   void *arg,
                   std::string &err);
    int execDelete(const ParsedQuery &q, std::string &err);

    static std::string makeCacheKey(const ParsedQuery &q);
};

#endif /* FLEXQL_EXECUTOR_H */
