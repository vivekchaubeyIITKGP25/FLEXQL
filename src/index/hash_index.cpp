/*
 * FlexQL: Hash index implementation.
 */

#include "hash_index.h"

#include <cstdint>

HashIndex::HashIndex(size_t reserveHint) {
    map_.reserve(reserveHint);
}

void HashIndex::insert(const std::string &key, size_t rowIdx) {
    map_[key] = rowIdx;
}

size_t HashIndex::lookup(const std::string &key) const {
    const auto it = map_.find(key);
    return (it == map_.end()) ? SIZE_MAX : it->second;
}

void HashIndex::remove(const std::string &key) {
    map_.erase(key);
}

void HashIndex::update(const std::string &key, size_t newIdx) {
    map_[key] = newIdx;
}

size_t HashIndex::size() const {
    return map_.size();
}

void HashIndex::clear() {
    map_.clear();
}
