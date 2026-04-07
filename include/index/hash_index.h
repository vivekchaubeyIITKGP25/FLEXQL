/*
 * FlexQL: Primary-key hash index.
 */

#ifndef FLEXQL_INDEX_H
#define FLEXQL_INDEX_H

#include <cstddef>
#include <string>
#include <unordered_map>

class HashIndex {
public:
    explicit HashIndex(size_t reserveHint = 1 << 20);

    void insert(const std::string &key, size_t rowIdx);
    size_t lookup(const std::string &key) const;
    void remove(const std::string &key);
    void update(const std::string &key, size_t newIdx);
    size_t size() const;
    void clear();

private:
    std::unordered_map<std::string, size_t> map_;
};

#endif /* FLEXQL_INDEX_H */
