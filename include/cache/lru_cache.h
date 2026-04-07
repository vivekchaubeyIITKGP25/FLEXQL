/*
 * FlexQL: LRU query cache.
 */

#ifndef FLEXQL_CACHE_H
#define FLEXQL_CACHE_H

#include <cstdint>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct CachedResult {
    std::vector<std::string>              columnNames;
    std::vector<std::vector<std::string>> rows;
    std::vector<int64_t>                  rowExpiresAt;
};

class LRUCache {
public:
    explicit LRUCache(size_t capacity = 1024, size_t maxBytes = 8 * 1024 * 1024);

    bool get(const std::string &key, CachedResult &result);
    void put(const std::string &key, const CachedResult &result);
    void invalidateTable(const std::string &tableName);
    void clear();

    size_t size() const;
    size_t capacity() const;
    size_t byteCapacity() const;
    size_t bytesUsed() const;
    uint64_t hits() const;
    uint64_t misses() const;

private:
    size_t capacity_;
    size_t maxBytes_;
    mutable std::mutex mutex_;

    struct Entry {
        std::string key;
        CachedResult result;
        size_t bytes = 0;
    };

    using KV = Entry;
    std::list<KV> lruList_;
    std::unordered_map<std::string, std::list<KV>::iterator> map_;
    size_t totalBytes_ = 0;
    uint64_t hits_ = 0;
    uint64_t misses_ = 0;
};

#endif /* FLEXQL_CACHE_H */
