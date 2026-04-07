/*
 * FlexQL: LRU cache implementation.
 */

#include "lru_cache.h"

#include <mutex>

namespace {

size_t resultBytes(const CachedResult &result) {
    size_t bytes = sizeof(CachedResult);
    for (const std::string &name : result.columnNames) {
        bytes += sizeof(std::string) + name.size();
    }
    for (const auto &row : result.rows) {
        bytes += sizeof(std::vector<std::string>);
        for (const std::string &value : row) {
            bytes += sizeof(std::string) + value.size();
        }
    }
    bytes += result.rowExpiresAt.size() * sizeof(int64_t);
    return bytes;
}

} // namespace

LRUCache::LRUCache(size_t capacity, size_t maxBytes)
    : capacity_(capacity), maxBytes_(maxBytes) {
    map_.reserve(capacity);
}

bool LRUCache::get(const std::string &key, CachedResult &result) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = map_.find(key);
    if (it == map_.end()) {
        ++misses_;
        return false;
    }

    lruList_.splice(lruList_.begin(), lruList_, it->second);
    result = it->second->result;
    ++hits_;
    return true;
}

void LRUCache::put(const std::string &key, const CachedResult &result) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t entryBytes = resultBytes(result);
    if (capacity_ == 0 || maxBytes_ == 0 || entryBytes > maxBytes_) {
        return;
    }

    const auto it = map_.find(key);
    if (it != map_.end()) {
        totalBytes_ -= it->second->bytes;
        it->second->result = result;
        it->second->bytes = entryBytes;
        totalBytes_ += entryBytes;
        lruList_.splice(lruList_.begin(), lruList_, it->second);
        return;
    }

    lruList_.push_front({key, result, entryBytes});
    map_[key] = lruList_.begin();
    totalBytes_ += entryBytes;

    while (!lruList_.empty() &&
           (lruList_.size() > capacity_ || totalBytes_ > maxBytes_)) {
        const auto last = std::prev(lruList_.end());
        totalBytes_ -= last->bytes;
        map_.erase(last->key);
        lruList_.erase(last);
    }
}

void LRUCache::invalidateTable(const std::string &tableName) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = lruList_.begin();
    while (it != lruList_.end()) {
        if (it->key.find(tableName) != std::string::npos) {
            totalBytes_ -= it->bytes;
            map_.erase(it->key);
            it = lruList_.erase(it);
        } else {
            ++it;
        }
    }
}

void LRUCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lruList_.clear();
    map_.clear();
    totalBytes_ = 0;
}

size_t LRUCache::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lruList_.size();
}

size_t LRUCache::capacity() const {
    return capacity_;
}

size_t LRUCache::byteCapacity() const {
    return maxBytes_;
}

size_t LRUCache::bytesUsed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalBytes_;
}

uint64_t LRUCache::hits() const {
    return hits_;
}

uint64_t LRUCache::misses() const {
    return misses_;
}
